package com.glammer.mge.editor

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ProjectsTest {
    // Captured from EditorSession::stateJson: three projects, "Level \"Two\"" open with an
    // unsaved rename.
    private val fixture = javaClass.classLoader!!.getResource("editor_state_projects.json")!!.readText()

    @Test
    fun parsesProjectState() {
        val s = EditorState.parse(fixture)
        assertEquals("Level \"Two\"", s.project.name)
        assertTrue(s.project.isOpen)
        assertTrue(s.dirty)
        assertEquals(listOf("My Game", "Level \"Two\"", "Space Race"), s.project.list.map { it.name })
        assertEquals(1763600120L, s.project.list[1].modified)
        assertFalse(EditorState.parse("{}").project.isOpen)
    }

    @Test
    fun namesMatchTheEngine() {
        assertEquals("my-game", Projects.slug("  My Game! "))
        assertEquals("space-race-2", Projects.slug("Space  -- Race 2"))
        assertEquals("lan", Projects.slug("élan"))
        assertTrue(Projects.isValidName("Hat \"red\""))
        assertFalse(Projects.isValidName("   "))
        assertFalse(Projects.isValidName("!!!"))
        assertFalse(Projects.isValidName("a\tb"))
        assertTrue(Projects.isValidName("x".repeat(48)))
        assertFalse(Projects.isValidName("x".repeat(49)))
        assertFalse(Projects.isValidName("é".repeat(25)))   // 50 bytes in UTF-8
        val s = EditorState.parse(fixture).project
        assertTrue(Projects.takenByOther(s, "my game"))
        assertFalse(Projects.takenByOther(s, "level two"))   // the open project itself
        assertFalse(Projects.takenByOther(s, "Brand New"))
    }

    @Test
    fun buildsProjectCommands() {
        assertEquals("save", EditorCommands.save())
        assertEquals("saveas Level 3", EditorCommands.saveAs(" Level\n3 "))
        assertEquals("open My Game", EditorCommands.open("My Game"))
        assertEquals("newproject", EditorCommands.newProject())
        assertEquals("newproject Moon", EditorCommands.newProject("Moon"))
        assertEquals("renameproject Moon Base", EditorCommands.renameProject("Moon Base"))
        assertEquals("deleteproject Moon", EditorCommands.deleteProject("Moon"))
        assertEquals(listOf("save", "open Space Race"), Projects.switchTo("Space Race", saveFirst = true))
        assertEquals(listOf("open Space Race"), Projects.switchTo("Space Race", saveFirst = false))
        assertEquals(listOf("save", "newproject Moon"), Projects.switchTo("Moon", saveFirst = true, create = true))
    }

    @Test
    fun formatsSaveTimes() {
        val now = 1_763_600_000L
        assertEquals("just now", Projects.ago(now - 5, now))
        assertEquals("5 min ago", Projects.ago(now - 300, now))
        assertEquals("3 h ago", Projects.ago(now - 3 * 3600, now))
        assertEquals("yesterday", Projects.ago(now - 30 * 3600, now))
        assertEquals("4 days ago", Projects.ago(now - 4 * 86400, now))
        assertEquals("2025-11-10", Projects.ago(now - 10 * 86400, now))
    }
}
