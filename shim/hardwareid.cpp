#include "hardwareid.h"
#include <sstream>

namespace {

std::string unescape(const std::string &s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '3' && s[i + 2] >= '0' &&
            s[i + 2] <= '7' && s[i + 3] >= '0' && s[i + 3] <= '7') {
            out += char((s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + (s[i + 3] - '0'));
            i += 3;
        } else {
            out += s[i];
        }
    }
    return out;
}

} // namespace

// mountinfo line: ID PARENT MAJ:MIN ROOT MOUNTPOINT OPTIONS [OPTIONAL...] - FSTYPE SOURCE SUPEROPTIONS
std::string mountSource(const std::string &mountinfo, const char *mountPoint)
{
    std::string found;
    std::istringstream lines(mountinfo);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string id, parent, dev, root, point, field;
        if (!(fields >> id >> parent >> dev >> root >> point))
            continue;
        if (unescape(point) != mountPoint)
            continue;
        while (fields >> field && field != "-") {
        }
        std::string type, source;
        if (field == "-" && fields >> type >> source)
            found = unescape(source);
    }
    return found;
}

std::string secretsFile(const char *home, const char *xdgConfigHome)
{
    std::string root;
    if (xdgConfigHome && *xdgConfigHome == '/')
        root = xdgConfigHome;
    else if (home && *home == '/')
        root = std::string(home) + "/.config";
    else
        return std::string();
    return root + "/Ableton/settings/secrets";
}
