package org.yux.lang.ide.lsp

import com.intellij.openapi.editor.colors.TextAttributesKey
import com.intellij.psi.PsiFile
import com.redhat.devtools.lsp4ij.client.features.LSPSemanticTokensFeature
import org.yux.lang.ide.highlighter.YuxColors

/**
 * 把 yux LSP server 发送的 semantic token 类型显式映到 YuxColors 里的可配置 key。
 * 用户在 Settings → Editor → Color Scheme → Yux 改色后，这里返回的 key 自动跟随。
 */
class YuxSemanticTokensFeature : LSPSemanticTokensFeature() {
    override fun getTextAttributesKey(
        tokenType: String,
        tokenModifiers: MutableList<String>,
        file: PsiFile
    ): TextAttributesKey? {
        val isDecl = "declaration" in tokenModifiers
        return when (tokenType) {
            "keyword"   -> YuxColors.KEYWORD
            "operator"  -> YuxColors.OPERATOR
            "string"    -> YuxColors.STRING
            "number"    -> YuxColors.NUMBER
            "comment"   -> YuxColors.LINE_COMMENT
            "variable"  -> YuxColors.VARIABLE
            "class"     -> YuxColors.CLASS
            "interface" -> YuxColors.INTERFACE
            "enum"       -> YuxColors.ENUM
            "enumMember" -> YuxColors.ENUM_MEMBER
            "function"  -> if (isDecl) YuxColors.FUNCTION_DECLARATION else YuxColors.FUNCTION_CALL
            "method"    -> if (isDecl) YuxColors.FUNCTION_DECLARATION else YuxColors.FUNCTION_CALL
            "property"  -> YuxColors.PROPERTY
            "parameter" -> YuxColors.PARAMETER
            "metadata"  -> YuxColors.METADATA
            else        -> super.getTextAttributesKey(tokenType, tokenModifiers, file)
        }
    }
}
