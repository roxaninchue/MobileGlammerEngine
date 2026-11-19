package com.glammer.mge.editor

import org.json.JSONArray
import org.json.JSONObject

/**
 * Editor panel state as the engine publishes it (EditorSession::stateJson, see
 * engine/include/mge/editor/editor_session.h). Plain data so Compose can diff it and unit tests
 * can build it from a string.
 */
data class EditorState(
    val revision: Long = -1,
    val undoLabel: String = "",
    val redoLabel: String = "",
    val dirty: Boolean = false,
    val locked: Boolean = false,
    val rows: List<TreeRow> = emptyList(),
    val selection: List<Int> = emptyList(),
    val inspector: InspectedEntity? = null,
    val gizmo: GizmoSettings = GizmoSettings(),
) {
    val canUndo get() = undoLabel.isNotEmpty()
    val canRedo get() = redoLabel.isNotEmpty()

    companion object {
        /** Parses the engine's JSON. Throws [org.json.JSONException] on malformed input. */
        fun parse(json: String): EditorState {
            val o = JSONObject(json)
            val rows = o.optJSONArray("rows").objects().map {
                TreeRow(
                    id = it.getInt("id"),
                    parent = it.optInt("p"),
                    depth = it.optInt("d"),
                    childCount = it.optInt("c"),
                    expanded = it.optBoolean("x"),
                    selected = it.optBoolean("s"),
                    name = it.optString("n"),
                )
            }
            val sel = o.optJSONArray("sel")?.let { a -> List(a.length()) { a.getInt(it) } } ?: emptyList()
            val insp = o.optJSONObject("insp")?.let { i ->
                InspectedEntity(
                    id = i.getInt("id"),
                    name = i.optString("name"),
                    components = i.optJSONArray("components").objects().map { c ->
                        InspectedComponent(
                            type = c.getString("type"),
                            removable = c.optBoolean("removable", true),
                            fields = c.optJSONArray("fields").objects().map { f ->
                                InspectedField(
                                    name = f.getString("name"),
                                    type = FieldType.of(f.optString("type")),
                                    hint = FieldHint.of(f.optString("hint")),
                                    value = f.optString("value"),
                                )
                            },
                        )
                    },
                    addable = i.optJSONArray("addable")?.let { a -> List(a.length()) { a.getString(it) } } ?: emptyList(),
                )
            }
            val gizmo = o.optJSONObject("gizmo")?.let { g ->
                GizmoSettings(
                    mode = GizmoMode.of(g.optString("mode")),
                    local = g.optBoolean("local"),
                    snap = g.optBoolean("snap"),
                    grid = g.optDouble("grid", 0.5).toFloat(),
                    angle = g.optDouble("angle", 15.0).toFloat(),
                    scaleStep = g.optDouble("scaleStep", 0.1).toFloat(),
                )
            } ?: GizmoSettings()
            return EditorState(
                revision = o.optLong("rev", -1),
                undoLabel = o.optString("undo"),
                redoLabel = o.optString("redo"),
                dirty = o.optBoolean("dirty"),
                locked = o.optBoolean("locked"),
                rows = rows,
                selection = sel,
                inspector = insp,
                gizmo = gizmo,
            )
        }

        private fun JSONArray?.objects(): List<JSONObject> =
            if (this == null) emptyList() else List(length()) { getJSONObject(it) }
    }
}

data class TreeRow(
    val id: Int,
    val parent: Int,
    val depth: Int,
    val childCount: Int,
    val expanded: Boolean,
    val selected: Boolean,
    val name: String,
)

data class InspectedEntity(
    val id: Int,
    val name: String,
    val components: List<InspectedComponent>,
    val addable: List<String>,
)

data class InspectedComponent(val type: String, val removable: Boolean, val fields: List<InspectedField>)

data class InspectedField(val name: String, val type: FieldType, val hint: FieldHint, val value: String) {
    /** Number of numeric parts the field edits as (Vec2 = 2, Vec4 = 4, scalars = 1, text = 0). */
    val parts: Int
        get() = when {
            hint == FieldHint.Bool || hint == FieldHint.Color -> 0
            type == FieldType.Vec2 -> 2
            type == FieldType.Vec4 -> 4
            type == FieldType.Str32 || type == FieldType.Unknown -> 0
            else -> 1
        }
}

enum class FieldType {
    F32, I32, U32, Vec2, Entity, Vec4, I16, U16, Str32, Unknown;

    val isInteger get() = this == I32 || this == U32 || this == I16 || this == U16 || this == Entity

    companion object {
        fun of(s: String) = entries.firstOrNull { it.name == s } ?: Unknown
    }
}

enum class FieldHint {
    None, Angle, Color, Bool, Hidden;

    companion object {
        fun of(s: String) = entries.firstOrNull { it.name == s } ?: None
    }
}

enum class GizmoMode(val protocol: String) {
    Move("move"), Rotate("rotate"), Scale("scale");

    companion object {
        fun of(s: String) = entries.firstOrNull { it.protocol == s } ?: Move
    }
}

/** Gizmo mode, axis space and snapping (grid in world units, angle in degrees, scale step). */
data class GizmoSettings(
    val mode: GizmoMode = GizmoMode.Move,
    val local: Boolean = false,
    val snap: Boolean = false,
    val grid: Float = 0.5f,
    val angle: Float = 15f,
    val scaleStep: Float = 0.1f,
)
