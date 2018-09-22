#include "absl/strings/match.h"
#include "common/Timer.h"
#include "lsp.h"
#include "spdlog/fmt/ostr.h"

using namespace std;

namespace sorbet {
namespace realmain {
namespace lsp {

void LSPLoop::processRequest(rapidjson::Document &d) {
    const LSPMethod method = LSPMethod::getByName({d["method"].GetString(), d["method"].GetStringLength()});

    ENFORCE(method.kind == LSPMethod::Kind::ClientInitiated || method.kind == LSPMethod::Kind::Both);

    if (!ensureInitialized(method, d)) {
        return;
    }
    if (method.isNotification) {
        logger->debug("Processing notification {} ", (string)method.name);
        if (method == LSPMethod::TextDocumentDidChange()) {
            prodCategoryCounterInc("lsp.requests.processed", "textDocument.didChange");
            Timer timeit(logger, "text_document_did_change");
            vector<shared_ptr<core::File>> files;
            auto &edits = d["params"];
            ENFORCE(edits.IsObject());
            /*
              {
              "textDocument":{"uri":"file:///Users/dmitry/stripe/pay-server/cibot/lib/cibot/gerald.rb","version":2},
                "contentChanges":[{"text":"..."}]
                */
            string uri(edits["textDocument"]["uri"].GetString(), edits["textDocument"]["uri"].GetStringLength());
            // TODO: if this is ever updated to support diffs, be aware that the coordinator thread should be
            // taught about it too: it merges consecutive TextDocumentDidChange
            if (absl::StartsWith(uri, rootUri)) {
                auto currentFileRef = initialGS->findFileByPath(remoteName2Local(uri));
                unique_ptr<core::File> file;
                if (currentFileRef.exists()) {
                    file = make_unique<core::File>((string)currentFileRef.data(*initialGS).path(),
                                                   (string)currentFileRef.data(*initialGS).source(),
                                                   core::File::Type::Normal);
                } else {
                    file = make_unique<core::File>(remoteName2Local(uri), "", core::File::Type::Normal);
                }

                for (auto &change : edits["contentChanges"].GetArray()) {
                    if (change.HasMember("range") && !change["range"].IsNull()) {
                        // incremental update
                        auto old = move(file);
                        string oldContent = (string)old->source();
                        core::Loc::Detail start, end;
                        start.line = change["range"]["start"]["line"].GetInt() + 1;
                        start.column = change["range"]["start"]["character"].GetInt() + 1;
                        end.line = change["range"]["end"]["line"].GetInt() + 1;
                        end.column = change["range"]["end"]["character"].GetInt() + 1;
                        auto startOffset = core::Loc::pos2Offset(*old, start);
                        auto endOffset = core::Loc::pos2Offset(*old, end);
                        string delta(change["text"].GetString(), change["text"].GetStringLength());
                        string newContent = oldContent.replace(startOffset, endOffset - startOffset, delta);
                        file = make_unique<core::File>((string)old->path(), move(newContent), core::File::Type::Normal);
                    } else {
                        // replace
                        auto old = move(file);
                        string newContent(change["text"].GetString(), change["text"].GetStringLength());
                        file = make_unique<core::File>((string)old->path(), move(newContent), core::File::Type::Normal);
                    }
                }

                logger->trace("Updating {} to have the following contents: {}", remoteName2Local(uri), file->source());

                files.emplace_back(move(file));

                tryFastPath(files);
                pushErrors();
            }
        }
        if (method == LSPMethod::TextDocumentDidOpen()) {
            prodCategoryCounterInc("requests.processed", "textDocument.didOpen");
            Timer timeit(logger, "text_document_did_open");
            vector<shared_ptr<core::File>> files;
            auto &edits = d["params"];
            ENFORCE(edits.IsObject());
            /*
              {
              "textDocument":{"uri":"file:///Users/dmitry/stripe/pay-server/cibot/lib/cibot/gerald.rb","version":2},
                "contentChanges":[{"text":"..."}]
                */
            string uri(edits["textDocument"]["uri"].GetString(), edits["textDocument"]["uri"].GetStringLength());
            string content(edits["textDocument"]["text"].GetString(), edits["textDocument"]["text"].GetStringLength());
            if (absl::StartsWith(uri, rootUri)) {
                files.emplace_back(
                    make_shared<core::File>(remoteName2Local(uri), move(content), core::File::Type::Normal));

                tryFastPath(files);
                pushErrors();
            }
        }
        if (method == LSPMethod::Initialized()) {
            prodCategoryCounterInc("requests.processed", "initialized");
            // initialize ourselves
            {
                Timer timeit(logger, "initial_index");
                reIndexFromFileSystem();
                vector<shared_ptr<core::File>> changedFiles;
                runSlowPath(move(changedFiles));
                ENFORCE(finalGs);
                pushErrors();
                this->globalStateHashes = computeStateHashes(finalGs->getFiles());
            }
        }
        if (method == LSPMethod::Exit()) {
            return;
        }
    } else {
        logger->debug("Processing request {}", method.name);
        // is request
        rapidjson::Value result;
        int errorCode = 0;
        string errorString;
        if (d.FindMember("cancelled") != d.MemberEnd()) {
            prodCounterInc("lsp.requests.cancelled");
            errorCode = (int)LSPErrorCodes::RequestCancelled;
            errorString = "Request was cancelled";
            sendError(d, errorCode, errorString);
        } else if (method == LSPMethod::Initialize()) {
            prodCategoryCounterInc("lsp.requests.processed", "initialize");
            result.SetObject();
            rootUri = string(d["params"]["rootUri"].GetString(), d["params"]["rootUri"].GetStringLength());
            string serverCap = "{\"capabilities\": "
                               "   {"
                               "       \"textDocumentSync\": 2, "
                               "       \"documentSymbolProvider\": true, "
                               "       \"workspaceSymbolProvider\": true, "
                               "       \"definitionProvider\": true, "
                               "       \"hoverProvider\":true,"
                               "       \"referencesProvider\":true,"
                               "       \"signatureHelpProvider\": { "
                               "           \"triggerCharacters\": [\"(\", \",\"]"
                               "       },"
                               "       \"completionProvider\": { "
                               "           \"triggerCharacters\": [\".\"]"
                               "       }"
                               "   }"
                               "}";
            rapidjson::Document temp;
            auto &r = temp.Parse(serverCap.c_str());
            ENFORCE(!r.HasParseError());
            result.CopyFrom(temp, alloc);
            sendResult(d, result);
        } else if (method == LSPMethod::Shutdown()) {
            prodCategoryCounterInc("lsp.requests.processed", "shutdown");
            // return default value: null
            sendResult(d, result);
        } else if (method == LSPMethod::TextDocumentDocumentSymbol()) {
            handleTextDocumentDocumentSymbol(result, d);
        } else if (method == LSPMethod::WorkspaceSymbols()) {
            handleWorkspaceSymbols(result, d);
        } else if (method == LSPMethod::TextDocumentDefinition()) {
            handleTextDocumentDefinition(result, d);
        } else if (method == LSPMethod::TextDocumentHover()) {
            handleTextDocumentHover(result, d);
        } else if (method == LSPMethod::TextDocumentCompletion()) {
            handleTextDocumentCompletion(result, d);
        } else if (method == LSPMethod::TextDocumentSignatureHelp()) {
            handleTextSignatureHelp(result, d);
        } else if (method == LSPMethod::TextDocumentRefernces()) {
            handleTextDocumentReferences(result, d);
        } else {
            ENFORCE(!method.isSupported, "failing a supported method");
            errorCode = (int)LSPErrorCodes::MethodNotFound;
            errorString = fmt::format("Unknown method: {}", method.name);
            sendError(d, errorCode, errorString);
        }
    }
}
} // namespace lsp
} // namespace realmain
} // namespace sorbet
