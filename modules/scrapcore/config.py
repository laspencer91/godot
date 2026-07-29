def can_build(env, platform):
    # Editor and template builds on the desktop platforms the game ships
    # (windows client/server, linux server). The module itself is portable
    # C++17; this list is about what we test, not what could compile.
    return platform in ("windows", "linuxbsd")


def configure(env):
    pass


def get_opts(platform):
    # D1 (docs/SCRAPCORE_ENGINE_MODULE_PLAN-2026-07-28.md): the canonical
    # ScrapCore source lives in the GAME repo beside its parity harness and
    # golden traces -- no vendored copy. The default resolves against the
    # engine root for the standard solo-dev layout (C:/Development/Engines/godot
    # beside C:/Development/games/scrapline).
    return [
        (
            "scrapcore_path",
            "Path to the game repo's native/ScrapCore (canonical ScrapCore source; see modules/scrapcore/README.md)",
            "../../games/scrapline/native/ScrapCore",
        ),
    ]
