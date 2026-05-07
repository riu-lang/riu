package org.yux.lang.ide.psi

import com.intellij.extapi.psi.PsiFileBase
import com.intellij.openapi.fileTypes.FileType
import com.intellij.psi.FileViewProvider
import org.yux.lang.ide.YuxFileType
import org.yux.lang.ide.YuxLanguage

class YuxFile(viewProvider: FileViewProvider) : PsiFileBase(viewProvider, YuxLanguage) {
    override fun getFileType(): FileType = YuxFileType
    override fun toString(): String = "Yux File"
}
