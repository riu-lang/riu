package org.yux.lang.ide.settings

import com.intellij.openapi.components.PersistentStateComponent
import com.intellij.openapi.components.Service
import com.intellij.openapi.components.State
import com.intellij.openapi.components.Storage
import com.intellij.openapi.components.service
import com.intellij.openapi.project.Project
import com.intellij.openapi.util.SystemInfo
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.Paths

@Service(Service.Level.PROJECT)
@State(name = "YuxSettings", storages = [Storage("yux.xml")])
class YuxSettings : PersistentStateComponent<YuxSettings.State> {
    data class State(
        // yux 主目录，期望布局：
        //   <home>/bin/yux(.exe)
        //   <home>/sdk/yux/core/*.yux
        var homePath: String = "",
    )

    private var state = State()

    override fun getState(): State = state
    override fun loadState(state: State) {
        this.state = state
    }

    var homePath: String
        get() = state.homePath
        set(value) {
            state.homePath = value.trim()
        }

    /**
     * 解析最终用于启动 LSP 的可执行文件路径：
     * - 配置了 homePath：返回 <home>/bin/yux-lsp(.exe)
     * - 否则：返回 "yux-lsp"（依赖 PATH）
     */
    fun resolveExecutable(): String {
        val home = state.homePath.trim()
        if (home.isNotEmpty()) {
            val exeName = if (SystemInfo.isWindows) "yux-lsp.exe" else "yux-lsp"
            val path: Path = Paths.get(home, "bin", exeName)
            if (Files.isRegularFile(path)) {
                return path.toAbsolutePath().toString()
            }
        }
        return "yux-lsp"
    }

    companion object {
        fun getInstance(project: Project): YuxSettings = project.service()
    }
}
