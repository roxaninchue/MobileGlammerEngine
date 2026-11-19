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
    fun gizmo(mode: GizmoMode) = "gizmo ${mode.protocol}"
    fun space(local: Boolean) = "space ${if (local) "local" else "world"}"
    fun snap(on: Boolean, grid: Float, angleDeg: Float, scaleStep: Float) =
        "snap ${if (on) 1 else 0} ${FieldValues.formatFloat(grid)} ${FieldValues.formatFloat(angleDeg)} ${FieldValues.formatFloat(scaleStep)}"
    fun undo() = "undo"
    fun play() = "play"
    fun pause() = "pause"
    fun step() = "step"
    fun stop() = "stop"
    fun save() = "save"
    fun saveAs(name: String) = "saveas ${clean(name)}"
    fun open(name: String) = "open ${clean(name)}"
    fun newProject(name: String = "") = "newproject ${clean(name)}".trimEnd()
    fun renameProject(name: String) = "renameproject ${clean(name)}"
    fun deleteProject(name: String) = "deleteproject ${clean(name)}"
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

/**
 * What the play controls offer for a [PlayInfo]: the main button plays, pauses or resumes; step
 * advances one physics step while paused; stop goes back to the scene as it was before play.
 */
class PlayControls(private val play: PlayInfo) {
    /** Main button: pause while playing, play otherwise (resumes when paused). */
    val primary: String get() = if (play.playing) EditorCommands.pause() else EditorCommands.play()
    val primaryGlyph: String get() = if (play.playing) "⏸" else "▶"
    val primaryLabel: String
        get() = when (play.state) {
            PlayState.Editing -> "Play"
            PlayState.Playing -> "Pause"
            PlayState.Paused -> "Resume"
        }
    val canStep: Boolean get() = play.paused
    val canStop: Boolean get() = play.active

    /** "12.34 s · 741 steps" while playing or paused, empty while editing. */
    val clock: String
        get() = if (!play.active) "" else String.format(Locale.ROOT, "%.2f s · %d steps", play.time, play.steps)
}

/**
 * Project rules shared with the engine (mge/editor/project.h): names, folder slugs, and the
 * protocol lines for switching projects without losing unsaved work.
 */
object Projects {
    const val MAX_NAME = 48

    /** Folder name the engine stores a project under: "My Game!" -> "my-game". */
    fun slug(name: String): String {
        val sb = StringBuilder()
        for (c in name.trim()) {
            val l = if (c in 'A'..'Z') c + ('a' - 'A') else c
            if (l in 'a'..'z' || l in '0'..'9') sb.append(l)
            else if (sb.isNotEmpty() && sb.last() != '-') sb.append('-')
        }
        return sb.toString().trimEnd('-')
    }

    fun isValidName(name: String): Boolean {
        val t = name.trim()
        return t.isNotEmpty() && t.toByteArray(Charsets.UTF_8).size <= MAX_NAME && t.none { it < ' ' || it == '\u007F' } && slug(t).isNotEmpty()
    }

    /** True when [name] is another project's (same folder), so "save as" would be refused. */
    fun takenByOther(state: ProjectState, name: String): Boolean {
        val s = slug(name)
        return s != slug(state.name) && state.list.any { slug(it.name) == s }
    }

    /**
     * Lines to leave the open project for [target] (open, or new when [create]): saved first
     * when [saveFirst], otherwise unsaved changes are dropped.
     */
    fun switchTo(target: String, saveFirst: Boolean, create: Boolean = false): List<String> =
        (if (saveFirst) listOf(EditorCommands.save()) else emptyList()) +
            (if (create) EditorCommands.newProject(target) else EditorCommands.open(target))

    /** "just now", "5 min ago", "3 h ago", "yesterday", "4 days ago", else the date (yyyy-MM-dd). */
    fun ago(modified: Long, nowSec: Long): String {
        val d = nowSec - modified
        return when {
            d < 60 -> "just now"
            d < 3600 -> "${d / 60} min ago"
            d < 86400 -> "${d / 3600} h ago"
            d < 2 * 86400 -> "yesterday"
            d < 7 * 86400 -> "${d / 86400} days ago"
            else -> java.time.Instant.ofEpochSecond(modified).atZone(java.time.ZoneOffset.UTC).toLocalDate().toString()
        }
    }
}
