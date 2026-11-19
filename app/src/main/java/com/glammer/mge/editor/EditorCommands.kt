package com.glammer.mge.editor

import java.util.Locale

/**
 * Builds the editor protocol lines the engine understands (EditorSession::handle). Pure
 * functions so they are unit tested on the JVM. Ids are editor ids; 0 = scene root / none.
 */
object EditorCommands {
    fun select(id: Int) = "select $id"
    fun toggle(id: Int) = "toggle $id"
    fun expand(id: Int, expanded: Boolean) = "expand $id ${if (expanded) 1 else 0}"
    fun create(parent: Int = 0, name: String = "Entity") = "create $parent ${clean(name).ifEmpty { "Entity" }}"
    fun rename(id: Int, name: String) = "rename $id ${clean(name)}"
    fun duplicate() = "duplicate"
    fun delete() = "delete"
    fun reparent(id: Int, parent: Int, index: Int = -1) = "reparent $id $parent $index"
    fun reorder(id: Int, index: Int) = "reorder $id $index"
    fun set(id: Int, type: String, field: String, value: String) = "set $id $type $field ${clean(value)}"
    /** A continuous edit (slider drag). Same [key] = one undo step; send [seal] when the finger lifts. */
    fun scrub(key: Long, id: Int, type: String, field: String, value: String) =
        "scrub $key $id $type $field ${clean(value)}"
    fun seal() = "seal"
    fun addComponent(id: Int, type: String) = "add $id $type"
    fun removeComponent(id: Int, type: String) = "remove $id $type"
    fun undo() = "undo"
    fun redo() = "redo"

    /** One line: newlines and tabs become spaces, surrounding blanks trimmed. */
    fun clean(s: String) = s.replace('\n', ' ').replace('\r', ' ').replace('\t', ' ').trim()
}

/** Field value text <-> numbers, matching the engine's inspector format (spaces between parts). */
object FieldValues {
    fun split(value: String): List<String> = value.trim().split(Regex("[\\s,;]+")).filter { it.isNotEmpty() }

    /** Replaces part [index] of a multi-part value ("1 2" -> "1 5"). Missing parts become 0. */
    fun withPart(value: String, parts: Int, index: Int, part: String): String {
        val p = split(value).toMutableList()
        while (p.size < parts) p.add("0")
        p[index] = part.trim()
        return p.take(parts).joinToString(" ")
    }

    /** Shortest round-trippable text for a float, always with '.' as decimal separator. */
    fun formatFloat(v: Float): String {
        if (v == v.toLong().toFloat() && kotlin.math.abs(v) < 1e7f) return v.toLong().toString()
        return String.format(Locale.ROOT, "%.4f", v).trimEnd('0').trimEnd('.')
    }

    fun parseFloat(s: String): Float? = s.trim().replace(',', '.').toFloatOrNull()?.takeIf { it.isFinite() }

    /** Value after dragging a scrubbed field by [dxDp] dp: 1 dp = [step] (angles: 1 degree per dp). */
    fun scrubbed(start: Float, dxDp: Float, type: FieldType, hint: FieldHint): String {
        val step = when {
            hint == FieldHint.Angle -> 1f
            type.isInteger -> 0.25f
            else -> 0.02f
        }
        val v = start + dxDp * step
        return if (type.isInteger) {
            val min = if (type == FieldType.U32 || type == FieldType.U16 || type == FieldType.Entity) 0L else Long.MIN_VALUE
            maxOf(min, kotlin.math.round(v).toLong()).toString()
        } else {
            formatFloat(kotlin.math.round(v * 100f) / 100f)
        }
    }

    /** "#RRGGBBAA" (or "#RRGGBB") -> ARGB Int for Compose Color, null if malformed. */
    fun colorArgb(hex: String): Int? {
        val h = hex.trim().removePrefix("#")
        if (h.length != 6 && h.length != 8) return null
        val v = h.toLongOrNull(16) ?: return null
        return if (h.length == 6) (0xFF000000L or v).toInt()
        else (((v and 0xFF) shl 24) or (v ushr 8)).toInt()
    }
}

/**
 * Hierarchy menu moves for [row], as protocol lines (null = not possible). Siblings are read
 * from the visible rows: an entity's siblings are always all visible together.
 */
class HierarchyMoves(rows: List<TreeRow>, row: TreeRow) {
    val up: String?
    val down: String?
    /** Into the previous sibling, as its last child. */
    val indent: String?
    /** Out of the parent, right after it. */
    val outdent: String?

    init {
        val siblings = rows.filter { it.parent == row.parent }
        val i = siblings.indexOfFirst { it.id == row.id }
        up = if (i > 0) EditorCommands.reorder(row.id, i - 1) else null
        down = if (i >= 0 && i < siblings.size - 1) EditorCommands.reorder(row.id, i + 1) else null
        indent = if (i > 0) EditorCommands.reparent(row.id, siblings[i - 1].id, -1) else null
        val parentRow = rows.firstOrNull { it.id == row.parent }
        outdent = parentRow?.let { p ->
            val parentIndex = rows.filter { it.parent == p.parent }.indexOfFirst { it.id == p.id }
            EditorCommands.reparent(row.id, p.parent, parentIndex + 1)
        }
    }
}
