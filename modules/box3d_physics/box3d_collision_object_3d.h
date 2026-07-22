/**************************************************************************/
/*  box3d_collision_object_3d.h                                            */
/**************************************************************************/

#pragma once

#include "core/object/object_id.h"
#include "core/templates/rid.h"

class Box3DSpace3D;

class Box3DCollisionObject3D {
public:
	enum Type {
		TYPE_BODY,
		TYPE_AREA,
	};

private:
	Type type;
	bool ray_pickable = true;

public:
	explicit Box3DCollisionObject3D(Type p_type) :
			type(p_type) {}
	virtual ~Box3DCollisionObject3D() = default;

	Type get_type() const { return type; }
	void set_ray_pickable(bool p_enable) { ray_pickable = p_enable; }
	bool is_ray_pickable() const { return ray_pickable; }
	virtual RID get_rid() const = 0;
	virtual ObjectID get_instance_id() const = 0;
	virtual Box3DSpace3D *get_space() const = 0;
	virtual void shapes_changed() = 0;
};
