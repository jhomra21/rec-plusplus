#pragma once

#include <string>

namespace rec::core {

// Turns a finished fragmented MP4 (a moov with mvex, then moof/mdat pairs) into a regular
// one in place: a complete moov goes at the end and the old moov, the fragment headers and
// mfra become free boxes, so the media data stays where it is. A file without fragments is
// left alone. Empty on success.
std::string consolidate_fragments(const std::string& path);

}  // namespace rec::core
