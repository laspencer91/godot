/**************************************************************************/
/*  editor_component_theme.h                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/

#pragma once

#include "editor/themes/editor_theme_manager.h"

class EditorComponentTheme {
public:
	static void populate(const Ref<EditorTheme> &p_theme, const EditorThemeManager::ThemeConfiguration &p_config);
};
