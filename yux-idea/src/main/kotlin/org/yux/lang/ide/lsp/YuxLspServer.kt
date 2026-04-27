package org.yux.lang.ide.lsp

import com.intellij.openapi.project.Project
import com.redhat.devtools.lsp4ij.server.ProcessStreamConnectionProvider
import org.yux.lang.ide.settings.YuxSettings

class YuxLspServer(project: Project) : ProcessStreamConnectionProvider() {
    init {
        val settings = YuxSettings.getInstance(project)
        val executable = settings.resolveExecutable()
        val cwd = project.basePath
        super.setCommands(listOf(executable, "lsp"))
        if (cwd != null) {
            super.setWorkingDirectory(cwd)
        }
        // 把 yux 主目录透传给子进程，便于编译器在非默认布局下定位 sdk/。
        if (settings.homePath.isNotEmpty()) {
            super.setUserEnvironmentVariables(mapOf("YUX_HOME" to settings.homePath))
        }
    }
}
