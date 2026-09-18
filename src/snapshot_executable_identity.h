#pragma once
#include <cstddef>
#include <string>
namespace snapshot_profiles::identity {
std::string HexDigest(const unsigned char *bytes, size_t size);
std::string ReadProfileFile(const std::string &path);
std::string MeasureRunningExecutable();
// Pure ELF inspection, also used on unexecuted private mutation fixtures.
// This cannot select or override the image measured by Profiles::Load.
void InspectExecutable(int fd);
}
