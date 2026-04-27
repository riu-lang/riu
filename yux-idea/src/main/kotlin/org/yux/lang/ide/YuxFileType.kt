package org.yux.lang.ide

import com.intellij.openapi.fileTypes.LanguageFileType
import javax.swing.Icon

object YuxFileType : LanguageFileType(YuxLanguage) {
    override fun getName(): String = "Yux File"
    override fun getDescription(): String = "Yux language source file"
    override fun getDefaultExtension(): String = "yux"
    override fun getIcon(): Icon = YuxIcons.FILE
}
