#include "Loc.h"
#include "Context.h"

#include <algorithm>
#include <iterator>

namespace ruby_typer {
namespace core {

using namespace std;

Loc Loc::join(Loc other) {
    if (this->is_none()) {
        return other;
    }
    if (other.is_none()) {
        return *this;
    }
    ENFORCE(this->file == other.file, "joining locations from different files");

    return Loc{this->file, min(this->begin_pos, other.begin_pos), max(this->end_pos, other.end_pos)};
}

Loc::Detail Loc::offset2Pos(core::FileRef source, u4 off, const core::GlobalState &gs) {
    Loc::Detail pos;

    const core::File &file = source.data(gs);
    ENFORCE(off <= file.source().size(), "file offset out of bounds");
    auto it = std::lower_bound(file.line_breaks.begin(), file.line_breaks.end(), off);
    if (it == file.line_breaks.begin()) {
        pos.line = 1;
        pos.column = off + 1;
        return pos;
    }
    --it;
    pos.line = (it - file.line_breaks.begin()) + 2;
    pos.column = off - *it;
    return pos;
}

pair<Loc::Detail, Loc::Detail> Loc::position(const core::GlobalState &gs) {
    Loc::Detail begin(offset2Pos(this->file, begin_pos, gs));
    Loc::Detail end(offset2Pos(this->file, end_pos, gs));
    return make_pair(begin, end);
}

void printTabs(stringstream &to, int count) {
    int i = 0;
    while (i < count) {
        to << "  ";
        i++;
    }
}

string Loc::toString(const core::GlobalState &gs, int tabs) {
    stringstream buf;
    absl::string_view source = this->file.data(gs).source();
    auto pos = this->position(gs);
    auto endstart = make_reverse_iterator(source.begin() + this->begin_pos);
    auto beginstart = make_reverse_iterator(source.begin());
    auto start = find(endstart, beginstart, '\n');

    auto end = find(source.begin() + this->end_pos, source.end(), '\n');

    auto offset1 = beginstart - start;
    auto offset2 = end - source.begin();
    string outline(source.begin() + (offset1), source.begin() + (offset2));
    printTabs(buf, tabs);
    buf << outline;
    if (pos.second.line == pos.first.line) {
        // add squigly
        buf << endl;
        printTabs(buf, tabs);
        int p;
        for (p = 1; p < pos.first.column; p++) {
            buf << " ";
        }
        for (; p < pos.second.column; p++) {
            buf << "^";
        }
    }
    return buf.str();
}

bool Loc::operator==(const Loc &rhs) const {
    return file == rhs.file && begin_pos == rhs.begin_pos && end_pos == rhs.end_pos;
}

bool Loc::operator!=(const Loc &rhs) const {
    return !(rhs == *this);
}

} // namespace core
} // namespace ruby_typer
