def can_build(env, platform):
    return not env["disable_physics_3d"]


def configure(env):
    pass


def get_doc_classes():
    return [
        "Box3DCharacterMover",
        "Box3DDirectSpaceState3D",
        "Box3DPhysics",
        "Box3DSurfaceMap",
        "Box3DSurfaceMaterial",
        "Box3DSurfaceMaterialLibrary",
        "Box3DSurfaceOverride3D",
    ]


def get_doc_path():
    return "doc_classes"
