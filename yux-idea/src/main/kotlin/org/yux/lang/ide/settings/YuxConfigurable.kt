package org.yux.lang.ide.settings

import com.intellij.openapi.fileChooser.FileChooserDescriptorFactory
import com.intellij.openapi.options.Configurable
import com.intellij.openapi.project.Project
import com.intellij.openapi.ui.TextFieldWithBrowseButton
import com.intellij.openapi.util.SystemInfo
import com.intellij.ui.components.JBLabel
import com.intellij.util.ui.FormBuilder
import java.nio.file.Files
import java.nio.file.Paths
import javax.swing.JComponent
import javax.swing.JPanel

/**
 * Settings → Languages & Frameworks → Yux
 *
 * 配置 yux 主目录（Home），插件会以 <home>/bin/yux(.exe) 启动 LSP 服务，
 * 同时该目录下应包含 sdk/ 等子目录，供编译器查找运行时。
 */
class YuxConfigurable(private val project: Project) : Configurable {
    private var panel: JPanel? = null
    private val homeField = TextFieldWithBrowseButton().apply {
        addBrowseFolderListener(
            project,
            FileChooserDescriptorFactory.createSingleFolderDescriptor()
                .withTitle("Yux Home Directory")
                .withDescription("选择 yux 安装根目录（包含 bin/、sdk/）"),
        )
    }
    private val statusLabel = JBLabel()

    override fun getDisplayName(): String = "Yux"

    override fun createComponent(): JComponent {
        homeField.textField.document.addDocumentListener(
            object : javax.swing.event.DocumentListener {
                override fun insertUpdate(e: javax.swing.event.DocumentEvent?) = refreshStatus()
                override fun removeUpdate(e: javax.swing.event.DocumentEvent?) = refreshStatus()
                override fun changedUpdate(e: javax.swing.event.DocumentEvent?) = refreshStatus()
            },
        )
        val built = FormBuilder.createFormBuilder()
            .addLabeledComponent(JBLabel("Yux home:"), homeField, 1, false)
            .addComponentToRightColumn(statusLabel, 1)
            .addComponentFillVertically(JPanel(), 0)
            .panel
        panel = built
        return built
    }

    override fun isModified(): Boolean =
        homeField.text.trim() != YuxSettings.getInstance(project).homePath

    override fun apply() {
        YuxSettings.getInstance(project).homePath = homeField.text.trim()
    }

    override fun reset() {
        homeField.text = YuxSettings.getInstance(project).homePath
        refreshStatus()
    }

    override fun disposeUIResources() {
        panel = null
    }

    private fun refreshStatus() {
        val home = homeField.text.trim()
        if (home.isEmpty()) {
            statusLabel.text = "未设置，将使用 PATH 中的 yux"
            return
        }
        val exeName = if (SystemInfo.isWindows) "yux.exe" else "yux"
        val exe = Paths.get(home, "bin", exeName)
        val sdk = Paths.get(home, "sdk")
        val parts = buildList {
            add(if (Files.isRegularFile(exe)) "✔ bin/$exeName" else "✘ bin/$exeName 未找到")
            add(if (Files.isDirectory(sdk)) "✔ sdk/" else "✘ sdk/ 未找到")
        }
        statusLabel.text = parts.joinToString("    ")
    }
}
