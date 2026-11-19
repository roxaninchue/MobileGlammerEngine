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
import androidx.compose.foundation.layout.width
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
    Column(modifier) {
        EditorToolbar(
            state, send,
            showHierarchy, { showHierarchy = !showHierarchy },
            showInspector, { showInspector = !showInspector },
        )
        Row(Modifier.fillMaxWidth().weight(1f)) {
            if (showHierarchy) HierarchyPanel(state, send, Modifier.width(220.dp).fillMaxHeight())
            Spacer(Modifier.weight(1f))
            val insp = state.inspector
            if (showInspector && insp != null) InspectorPanel(insp, state.locked, send, Modifier.width(300.dp).fillMaxHeight())
        }
    }
}

@Composable
private fun EditorToolbar(
    state: EditorState,
    send: (String) -> Unit,
    hierarchyShown: Boolean,
    toggleHierarchy: () -> Unit,
    inspectorShown: Boolean,
    toggleInspector: () -> Unit,
) {
    val editable = !state.locked
    Row(
        Modifier.fillMaxWidth().background(PanelBg).padding(horizontal = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        ToolButton("☰", hierarchyShown, onClick = toggleHierarchy)
        ToolButton("↶", state.canUndo && editable) { send(EditorCommands.undo()) }
        ToolButton("↷", state.canRedo && editable) { send(EditorCommands.redo()) }
        ToolButton("+", editable) { send(EditorCommands.create(0, "Entity")) }
        ToolButton("⧉", editable && state.selection.isNotEmpty()) { send(EditorCommands.duplicate()) }
        ToolButton("✕", editable && state.selection.isNotEmpty()) { send(EditorCommands.delete()) }
        Spacer(Modifier.weight(1f))
        Text(
            if (state.canUndo) "Undo: ${state.undoLabel}" else "",
            color = TextDim, fontSize = 11.sp, maxLines = 1, overflow = TextOverflow.Ellipsis,
            modifier = Modifier.padding(horizontal = 8.dp),
        )
        if (state.dirty) Text("●", color = Accent, fontSize = 12.sp)
        ToolButton("⚙", inspectorShown, onClick = toggleInspector)
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
