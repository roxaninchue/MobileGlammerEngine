package com.glammer.mge.editor

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TextField
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import java.util.concurrent.atomic.AtomicLong

private val PanelBg = Color(0xE01B1E24)
private val RowSelected = Color(0xFF3B5B8C)
private val TextMain = Color(0xFFE8EAED)
private val TextDim = Color(0xFF9AA0A6)
private val FieldBg = Color(0xFF2A2E36)
private val Accent = Color(0xFFFFA726)
private val PlayTint = Color(0xE0213A2A)     // toolbar while playing: the scene is the game's
private val PausedTint = Color(0xE03A3421)   // toolbar while paused
private val PlayGreen = Color(0xFF66BB6A)

/** One scrub key per drag gesture: every scrub with the same key is one undo step. */
private val scrubKeys = AtomicLong(1)

/**
 * Editor chrome over the engine viewport (M2 #22): toolbar, hierarchy (left) and inspector
 * (right). Every edit is a protocol line sent with [send]; the engine applies it on its render
 * thread and publishes a new [EditorState], so the panels never hold scene data of their own.
 */
@Composable
fun EditorOverlay(state: EditorState, send: (String) -> Unit, modifier: Modifier = Modifier) {
    var showHierarchy by remember { mutableStateOf(true) }
    var showInspector by remember { mutableStateOf(true) }
    var showProjects by remember { mutableStateOf(false) }
    Column(modifier) {
        EditorToolbar(
            state, send,
            showHierarchy, { showHierarchy = !showHierarchy },
            showInspector, { showInspector = !showInspector },
            openProjects = { showProjects = true },
        )
        Row(Modifier.fillMaxWidth().weight(1f)) {
            if (showHierarchy) HierarchyPanel(state, send, Modifier.width(220.dp).fillMaxHeight())
            Spacer(Modifier.weight(1f))
            val insp = state.inspector
            if (showInspector && insp != null) InspectorPanel(insp, state.locked, send, Modifier.width(300.dp).fillMaxHeight())
        }
    }
    if (showProjects) ProjectsDialog(state, send, onDismiss = { showProjects = false })
}

@Composable
private fun EditorToolbar(
    state: EditorState,
    send: (String) -> Unit,
    hierarchyShown: Boolean,
    toggleHierarchy: () -> Unit,
    inspectorShown: Boolean,
    toggleInspector: () -> Unit,
    openProjects: () -> Unit,
) {
    val editable = !state.locked
    val bar = when (state.play.state) {
        PlayState.Editing -> PanelBg
        PlayState.Playing -> PlayTint
        PlayState.Paused -> PausedTint
    }
    Row(
        Modifier.fillMaxWidth().background(bar).padding(horizontal = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        ToolButton("☰", hierarchyShown, onClick = toggleHierarchy)
        // Project: name opens the projects dialog; ⤓ saves (also while playing: the scene from before play).
        TextButton(onClick = openProjects) {
            Text(
                (state.project.name.ifEmpty { "Projects" }) + " ▾", color = TextMain, fontSize = 13.sp,
                maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.widthIn(max = 140.dp),
            )
        }
        ToolButton("⤓", state.project.isOpen && state.dirty) { send(EditorCommands.save()) }
        ToolButton("↶", state.canUndo && editable) { send(EditorCommands.undo()) }
        ToolButton("↷", state.canRedo && editable) { send(EditorCommands.redo()) }
        ToolButton("+", editable) { send(EditorCommands.create(0, "Entity")) }
        ToolButton("⧉", editable && state.selection.isNotEmpty()) { send(EditorCommands.duplicate()) }
        ToolButton("✕", editable && state.selection.isNotEmpty()) { send(EditorCommands.delete()) }
        Spacer(Modifier.width(8.dp))
        // Gizmo: mode, snapping, axis space.
        for ((mode, glyph) in listOf(GizmoMode.Move to "✥", GizmoMode.Rotate to "⟳", GizmoMode.Scale to "⤡")) {
            ToggleButton(glyph, state.gizmo.mode == mode, editable) { send(EditorCommands.gizmo(mode)) }
        }
        val g = state.gizmo
        ToggleButton("#", g.snap, editable) { send(EditorCommands.snap(!g.snap, g.grid, g.angle, g.scaleStep)) }
        ToggleButton(if (g.local) "L" else "W", g.local, editable) { send(EditorCommands.space(!g.local)) }
        Spacer(Modifier.weight(1f))
        PlayGroup(state.play, send)
        Spacer(Modifier.weight(1f))
        Text(
            if (state.canUndo && !state.play.active) "Undo: ${state.undoLabel}" else "",
            color = TextDim, fontSize = 11.sp, maxLines = 1, overflow = TextOverflow.Ellipsis,
            modifier = Modifier.padding(horizontal = 8.dp),
        )
        if (state.dirty) Text("●", color = Accent, fontSize = 12.sp)
        ToolButton("⚙", inspectorShown, onClick = toggleInspector)
    }
}

/**
 * Play controls (M2 #24): play / pause / resume, step one physics step while paused, stop back
 * to the scene as it was before play, and the simulated clock.
 */
@Composable
private fun PlayGroup(play: PlayInfo, send: (String) -> Unit) {
    val c = PlayControls(play)
    Row(verticalAlignment = Alignment.CenterVertically) {
        TextButton(onClick = { send(c.primary) }) {
            Text(c.primaryGlyph, fontSize = 18.sp, color = if (play.playing) Accent else PlayGreen)
        }
        ToolButton("⏭", c.canStep) { send(EditorCommands.step()) }
        ToolButton("■", c.canStop) { send(EditorCommands.stop()) }
        if (play.active) {
            Text(
                c.clock, color = if (play.playing) PlayGreen else Accent, fontSize = 11.sp, maxLines = 1,
                modifier = Modifier.padding(start = 4.dp),
            )
        }
    }
}

@Composable
private fun ToggleButton(label: String, on: Boolean, enabled: Boolean, onClick: () -> Unit) {
    TextButton(onClick = onClick, enabled = enabled) {
        Text(
            label, fontSize = 16.sp,
            color = when {
                !enabled -> TextDim.copy(alpha = 0.4f)
                on -> Accent
                else -> TextMain
            },
        )
    }
}

@Composable
private fun ToolButton(label: String, enabled: Boolean, onClick: () -> Unit) {
    TextButton(onClick = onClick, enabled = enabled) {
        Text(label, fontSize = 18.sp, color = if (enabled) TextMain else TextDim.copy(alpha = 0.4f))
    }
}

// ------------------------------------------------------------------ hierarchy

@Composable
fun HierarchyPanel(state: EditorState, send: (String) -> Unit, modifier: Modifier = Modifier) {
    var renaming by remember { mutableStateOf<TreeRow?>(null) }
    LazyColumn(modifier.background(PanelBg).padding(vertical = 4.dp)) {
        items(state.rows, key = { it.id }) { row ->
            HierarchyRow(row, state, send, onRename = { renaming = row })
        }
    }
    renaming?.let { row ->
        RenameDialog(row.name, onDismiss = { renaming = null }) {
            send(EditorCommands.rename(row.id, it))
            renaming = null
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun HierarchyRow(row: TreeRow, state: EditorState, send: (String) -> Unit, onRename: () -> Unit) {
    var menu by remember { mutableStateOf(false) }
    val editable = !state.locked
    Box {
        Row(
            Modifier
                .fillMaxWidth()
                .background(if (row.selected) RowSelected else Color.Transparent)
                .combinedClickable(
                    onClick = { send(EditorCommands.select(row.id)) },
                    onLongClick = { send(EditorCommands.select(row.id)); menu = true },
                )
                .padding(start = (8 + row.depth * 14).dp, end = 8.dp)
                .height(32.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            val arrow = when {
                row.childCount == 0 -> " "
                row.expanded -> "▾"
                else -> "▸"
            }
            Text(
                arrow, color = TextDim, fontSize = 14.sp,
                modifier = Modifier.width(18.dp).combinedClickable(
                    onClick = { if (row.childCount > 0) send(EditorCommands.expand(row.id, !row.expanded)) },
                ),
            )
            Text(row.name, color = TextMain, fontSize = 13.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
        DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
            val moves = HierarchyMoves(state.rows, row)
            MenuItem("Rename", editable) { menu = false; onRename() }
            MenuItem("Add child", editable) { menu = false; send(EditorCommands.create(row.id, "Entity")) }
            MenuItem("Duplicate", editable) { menu = false; send(EditorCommands.duplicate()) }
            MenuItem("Delete", editable) { menu = false; send(EditorCommands.delete()) }
            MenuItem("Move up", editable && moves.up != null) { menu = false; moves.up?.let(send) }
            MenuItem("Move down", editable && moves.down != null) { menu = false; moves.down?.let(send) }
            MenuItem("Indent (into previous)", editable && moves.indent != null) { menu = false; moves.indent?.let(send) }
            MenuItem("Outdent (to parent's level)", editable && moves.outdent != null) { menu = false; moves.outdent?.let(send) }
        }
    }
}

@Composable
private fun MenuItem(text: String, enabled: Boolean, onClick: () -> Unit) {
    DropdownMenuItem(text = { Text(text) }, onClick = onClick, enabled = enabled)
}

@Composable
private fun RenameDialog(current: String, onDismiss: () -> Unit, onDone: (String) -> Unit) {
    var text by remember { mutableStateOf(current) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Rename") },
        text = {
            TextField(
                value = text, onValueChange = { text = it.take(31) }, singleLine = true,
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
                keyboardActions = KeyboardActions(onDone = { if (text.isNotBlank()) onDone(text) }),
            )
        },
        confirmButton = { TextButton(onClick = { if (text.isNotBlank()) onDone(text) }) { Text("OK") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

// ------------------------------------------------------------------ inspector

@Composable
fun InspectorPanel(entity: InspectedEntity, locked: Boolean, send: (String) -> Unit, modifier: Modifier = Modifier) {
    var addMenu by remember { mutableStateOf(false) }
    Column(modifier.background(PanelBg).verticalScroll(rememberScrollState()).padding(8.dp)) {
        CommitField(
            value = entity.name, enabled = !locked,
            textStyle = TextStyle(color = TextMain, fontSize = 15.sp, fontWeight = FontWeight.SemiBold),
            modifier = Modifier.fillMaxWidth(),
        ) { if (it.isNotBlank()) send(EditorCommands.rename(entity.id, it)) }
        Text("id ${entity.id}", color = TextDim, fontSize = 10.sp)
        for (c in entity.components) {
            Spacer(Modifier.height(8.dp))
            ComponentCard(entity.id, c, locked, send)
        }
        if (entity.addable.isNotEmpty() && !locked) {
            Spacer(Modifier.height(8.dp))
            Box {
                TextButton(onClick = { addMenu = true }) { Text("+ Add component", color = Accent) }
                DropdownMenu(expanded = addMenu, onDismissRequest = { addMenu = false }) {
                    for (t in entity.addable) MenuItem(t, true) { addMenu = false; send(EditorCommands.addComponent(entity.id, t)) }
                }
            }
        }
    }
}

@Composable
private fun ComponentCard(id: Int, c: InspectedComponent, locked: Boolean, send: (String) -> Unit) {
    Column(Modifier.fillMaxWidth().border(1.dp, FieldBg, RoundedCornerShape(6.dp)).padding(8.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(c.type, color = TextMain, fontSize = 13.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f))
            if (c.removable && !locked) {
                Text(
                    "✕", color = TextDim, fontSize = 14.sp,
                    modifier = Modifier.combinedClickableCompat { send(EditorCommands.removeComponent(id, c.type)) }.padding(4.dp),
                )
            }
        }
        for (f in c.fields) FieldRow(id, c.type, f, locked, send)
    }
}

@OptIn(ExperimentalFoundationApi::class)
private fun Modifier.combinedClickableCompat(onClick: () -> Unit) = this.combinedClickable(onClick = onClick)

@Composable
private fun FieldRow(id: Int, type: String, f: InspectedField, locked: Boolean, send: (String) -> Unit) {
    Row(Modifier.fillMaxWidth().padding(top = 4.dp), verticalAlignment = Alignment.CenterVertically) {
        val label = if (f.hint == FieldHint.Angle) "${f.name}°" else f.name
        if (f.parts == 1) {
            // Drag the label to scrub the value.
            ScrubLabel(label, f, locked) { v, key -> send(EditorCommands.scrub(key, id, type, f.name, v)) }
        } else {
            Text(label, color = TextDim, fontSize = 12.sp, maxLines = 1, modifier = Modifier.width(78.dp))
        }
        when {
            f.hint == FieldHint.Bool -> Switch(
                checked = f.value == "true", enabled = !locked,
                onCheckedChange = { send(EditorCommands.set(id, type, f.name, if (it) "true" else "false")) },
            )
            f.hint == FieldHint.Enum -> EnumField(f, !locked, Modifier.weight(1f)) {
                send(EditorCommands.set(id, type, f.name, it))
            }
            f.hint == FieldHint.Color -> {
                val argb = FieldValues.colorArgb(f.value)
                Box(Modifier.size(18.dp).background(if (argb != null) Color(argb) else Color.Magenta, RoundedCornerShape(3.dp)))
                Spacer(Modifier.width(6.dp))
                CommitField(f.value, !locked, Modifier.weight(1f)) { send(EditorCommands.set(id, type, f.name, it)) }
            }
            f.parts >= 2 -> Row(Modifier.weight(1f), horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                val parts = FieldValues.split(f.value)
                for (i in 0 until f.parts) {
                    CommitField(parts.getOrElse(i) { "0" }, !locked, Modifier.weight(1f), KeyboardType.Decimal) {
                        send(EditorCommands.set(id, type, f.name, FieldValues.withPart(f.value, f.parts, i, it)))
                    }
                }
            }
            f.parts == 1 -> CommitField(
                f.value, !locked, Modifier.weight(1f),
                if (f.type.isInteger) KeyboardType.Number else KeyboardType.Decimal,
            ) { send(EditorCommands.set(id, type, f.name, it)) }
            else -> CommitField(f.value, !locked, Modifier.weight(1f)) { send(EditorCommands.set(id, type, f.name, it)) }
        }
    }
}

/** Enum field: the current option, tap for a menu of the others. */
@Composable
private fun EnumField(f: InspectedField, enabled: Boolean, modifier: Modifier, onPick: (String) -> Unit) {
    var open by remember { mutableStateOf(false) }
    Box(modifier) {
        Text(
            "${f.value} ▾", color = if (enabled) TextMain else TextDim, fontSize = 13.sp, maxLines = 1,
            modifier = Modifier
                .fillMaxWidth()
                .background(FieldBg, RoundedCornerShape(4.dp))
                .combinedClickableCompat { if (enabled) open = true }
                .padding(horizontal = 6.dp, vertical = 4.dp),
        )
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            for (o in f.options) MenuItem(o, true) { open = false; if (o != f.value) onPick(o) }
        }
    }
}

@Composable
private fun ScrubLabel(label: String, f: InspectedField, locked: Boolean, onScrub: (String, Long) -> Unit) {
    val density = LocalDensity.current.density
    val current by rememberUpdatedState(f)
    var key by remember { mutableStateOf(0L) }
    var start by remember { mutableStateOf(0f) }
    var dragDp by remember { mutableStateOf(0f) }
    Text(
        label, color = if (key != 0L) Accent else TextDim, fontSize = 12.sp, maxLines = 1,
        modifier = Modifier.width(78.dp).pointerInput(locked) {
            if (locked) return@pointerInput
            detectHorizontalDragGestures(
                onDragStart = {
                    key = scrubKeys.getAndIncrement()
                    start = FieldValues.parseFloat(current.value) ?: 0f
                    dragDp = 0f
                },
                onDragEnd = { key = 0L },
                onDragCancel = { key = 0L },
            ) { change, dx ->
                change.consume()
                dragDp += dx / density
                onScrub(FieldValues.scrubbed(start, dragDp, current.type, current.hint), key)
            }
        },
    )
}

/**
 * Text field that keeps its own text while focused and sends it on Done or when focus leaves,
 * so engine updates never fight the keyboard.
 */
@Composable
private fun CommitField(
    value: String,
    enabled: Boolean,
    modifier: Modifier = Modifier,
    keyboardType: KeyboardType = KeyboardType.Text,
    textStyle: TextStyle = TextStyle(color = TextMain, fontSize = 13.sp),
    onCommit: (String) -> Unit,
) {
    var text by remember(value) { mutableStateOf(value) }
    var focused by remember { mutableStateOf(false) }
    val commit = { if (text != value) onCommit(text) }
    BasicTextField(
        value = text,
        onValueChange = { text = it },
        enabled = enabled,
        singleLine = true,
        textStyle = textStyle,
        cursorBrush = SolidColor(Accent),
        keyboardOptions = KeyboardOptions(keyboardType = keyboardType, imeAction = ImeAction.Done),
        keyboardActions = KeyboardActions(onDone = { commit() }),
        modifier = modifier
            .background(FieldBg, RoundedCornerShape(4.dp))
            .padding(horizontal = 6.dp, vertical = 4.dp)
            .onFocusChanged {
                if (focused && !it.isFocused) commit()
                focused = it.isFocused
            },
    )
}

// ------------------------------------------------------------------ projects (M2 #25)

/** What the projects dialog is asking about on top of the list. */
private sealed interface ProjectPrompt {
    data object New : ProjectPrompt
    data object SaveAs : ProjectPrompt
    data object Rename : ProjectPrompt
    /** Leaving a project with unsaved changes for [target] (open it, or create it when [create]). */
    data class Unsaved(val target: String, val create: Boolean) : ProjectPrompt
    data class Delete(val name: String) : ProjectPrompt
}

/**
 * Projects (M2 #25): the open project (save, save as, rename), every other project on the
 * device newest first (tap to open, ✕ to delete) and a new project. Leaving a project with
 * unsaved changes asks first: save, don't save, or stay.
 */
@Composable
fun ProjectsDialog(state: EditorState, send: (String) -> Unit, onDismiss: () -> Unit) {
    var prompt by remember { mutableStateOf<ProjectPrompt?>(null) }
    val project = state.project
    val nowSec = System.currentTimeMillis() / 1000
    fun leaveFor(target: String, create: Boolean) {
        if (project.isOpen && state.dirty) prompt = ProjectPrompt.Unsaved(target, create)
        else { Projects.switchTo(target, saveFirst = false, create = create).forEach(send); onDismiss() }
    }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Projects") },
        text = {
            Column {
                if (project.isOpen) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(project.name, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
                        if (state.dirty) Text(" ● unsaved", color = Accent, fontSize = 12.sp)
                    }
                    Row {
                        TextButton(onClick = { send(EditorCommands.save()) }, enabled = state.dirty) { Text("Save") }
                        TextButton(onClick = { prompt = ProjectPrompt.SaveAs }) { Text("Save as…") }
                        TextButton(onClick = { prompt = ProjectPrompt.Rename }) { Text("Rename") }
                    }
                }
                val others = project.list.filter { Projects.slug(it.name) != Projects.slug(project.name) }
                if (others.isNotEmpty()) {
                    Text("Open", color = TextDim, fontSize = 12.sp, modifier = Modifier.padding(top = 8.dp, bottom = 4.dp))
                    LazyColumn(Modifier.heightIn(max = 260.dp)) {
                        items(others, key = { it.name }) { p ->
                            Row(
                                Modifier.fillMaxWidth().combinedClickableCompat { leaveFor(p.name, create = false) }.padding(vertical = 6.dp),
                                verticalAlignment = Alignment.CenterVertically,
                            ) {
                                Column(Modifier.weight(1f)) {
                                    Text(p.name, maxLines = 1, overflow = TextOverflow.Ellipsis)
                                    Text(Projects.ago(p.modified, nowSec), color = TextDim, fontSize = 11.sp)
                                }
                                Text(
                                    "✕", color = TextDim, fontSize = 14.sp,
                                    modifier = Modifier.combinedClickableCompat { prompt = ProjectPrompt.Delete(p.name) }.padding(8.dp),
                                )
                            }
                        }
                    }
                }
            }
        },
        confirmButton = { TextButton(onClick = { prompt = ProjectPrompt.New }) { Text("+ New project") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Close") } },
    )
    when (val p = prompt) {
        null -> Unit
        ProjectPrompt.New -> NameDialog("New project", "", project, checkTaken = false, onDismiss = { prompt = null }) {
            prompt = null
            leaveFor(it, create = true)
        }
        ProjectPrompt.SaveAs -> NameDialog("Save as", project.name, project, checkTaken = true, onDismiss = { prompt = null }) {
            send(EditorCommands.saveAs(it))
            prompt = null
        }
        ProjectPrompt.Rename -> NameDialog("Rename project", project.name, project, checkTaken = true, onDismiss = { prompt = null }) {
            send(EditorCommands.renameProject(it))
            prompt = null
        }
        is ProjectPrompt.Unsaved -> AlertDialog(
            onDismissRequest = { prompt = null },
            title = { Text("Save changes to ${project.name}?") },
            text = { Text("Your changes will be lost if you don't save them.") },
            confirmButton = {
                TextButton(onClick = { Projects.switchTo(p.target, saveFirst = true, create = p.create).forEach(send); prompt = null; onDismiss() }) {
                    Text("Save")
                }
            },
            dismissButton = {
                Row {
                    TextButton(onClick = { prompt = null }) { Text("Cancel") }
                    TextButton(onClick = { Projects.switchTo(p.target, saveFirst = false, create = p.create).forEach(send); prompt = null; onDismiss() }) {
                        Text("Don't save")
                    }
                }
            },
        )
        is ProjectPrompt.Delete -> AlertDialog(
            onDismissRequest = { prompt = null },
            title = { Text("Delete ${p.name}?") },
            text = { Text("The project and its scene are removed from this device. This can't be undone.") },
            confirmButton = { TextButton(onClick = { send(EditorCommands.deleteProject(p.name)); prompt = null }) { Text("Delete", color = Color(0xFFEF5350)) } },
            dismissButton = { TextButton(onClick = { prompt = null }) { Text("Cancel") } },
        )
    }
}

/** Project name prompt; explains why a name can't be used (empty, too long, taken). */
@Composable
private fun NameDialog(
    title: String,
    initial: String,
    project: ProjectState,
    checkTaken: Boolean,   // save as / rename must not collide; a new project gets a fresh name ("Moon 2")
    onDismiss: () -> Unit,
    onDone: (String) -> Unit,
) {
    var text by remember { mutableStateOf(initial) }
    val name = text.trim()
    val error = when {
        name.isEmpty() -> null
        !Projects.isValidName(name) -> "Use up to ${Projects.MAX_NAME} characters, with at least one letter or digit."
        checkTaken && Projects.takenByOther(project, name) -> "Another project has this name."
        else -> null
    }
    val ok = name.isNotEmpty() && error == null
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = {
            Column {
                TextField(
                    value = text, onValueChange = { text = it }, singleLine = true, isError = error != null,
                    placeholder = { Text("Name") },
                    keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
                    keyboardActions = KeyboardActions(onDone = { if (ok) onDone(name) }),
                )
                if (error != null) Text(error, color = Color(0xFFEF5350), fontSize = 12.sp, modifier = Modifier.padding(top = 4.dp))
            }
        },
        confirmButton = { TextButton(onClick = { onDone(name) }, enabled = ok) { Text("OK") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}
