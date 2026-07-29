/**************************************************************************/
/*  scrap_core_unit.cpp                                                   */
/**************************************************************************/

// Compiles the canonical ScrapCore motor implementation from the external
// game-repo path (decision D1 -- no vendored copy). The core root is on the
// include path (SCsub, `scrapcore_path`); routing the source through this
// wrapper TU keeps the object file inside the engine build tree instead of
// writing beside the game repo's sources.
#include <src/movement_motor.cpp>
