/*
 * In-player extraction of translatable strings (replaces the external
 * lcftrans binary). Parses RPG_RT.ldb, RPG_RT.lmt and all .lmu maps with
 * liblcf and returns the strings in the same shape the runtime
 * translation lookup sees them (whole message boxes / choice blocks,
 * control bytes stripped).
 */

#ifndef EP_MTOOL_EXTRACT_H
#define EP_MTOOL_EXTRACT_H

#include <string>

namespace MtoolExtract {

/**
 * Extract all translatable strings of the current game.
 * @return JSON array of {"c": context, "t": text, "ch": is_choice}.
 *         "c" is omitted when empty, "ch" only present when true.
 */
std::string ExtractAllJson();

} // namespace MtoolExtract

#endif
