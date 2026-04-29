package org.yux.lang.ide.highlighter

import com.intellij.openapi.fileTypes.SyntaxHighlighter
import com.intellij.openapi.options.colors.AttributesDescriptor
import com.intellij.openapi.options.colors.ColorDescriptor
import com.intellij.openapi.options.colors.ColorSettingsPage
import org.yux.lang.ide.YuxIcons
import javax.swing.Icon

class YuxColorSettingsPage : ColorSettingsPage {

    override fun getDisplayName(): String = "Yux"

    override fun getIcon(): Icon = YuxIcons.FILE

    override fun getHighlighter(): SyntaxHighlighter = YuxSyntaxHighlighter()

    override fun getAttributeDescriptors(): Array<AttributesDescriptor> = DESCRIPTORS

    override fun getColorDescriptors(): Array<ColorDescriptor> = ColorDescriptor.EMPTY_ARRAY

    override fun getDemoText(): String = """
        ; 行注释
        use yux.core.*

        struct N {
          v i32
        }

        N {
          fn N(v i32) {
            ${'$'}.v = v
          }

          fn plus(other N) N {
            N(${'$'}.v + other.v)
          }
        }

        fn main() {
          var n = N(1)
          var n2 = N(2)
          var n3 = n.plus(n2)
          println(n3.v.to_string())
        }
    """.trimIndent()

    override fun getAdditionalHighlightingTagToDescriptorMap(): Map<String, com.intellij.openapi.editor.colors.TextAttributesKey>? = null

    companion object {
        private val DESCRIPTORS = arrayOf(
            AttributesDescriptor("注释",       YuxColors.LINE_COMMENT),
            AttributesDescriptor("字符串",     YuxColors.STRING),
            AttributesDescriptor("代码点",     YuxColors.CODE_POINT),
            AttributesDescriptor("数字",       YuxColors.NUMBER),
            AttributesDescriptor("关键字",     YuxColors.KEYWORD),
            AttributesDescriptor("运算符",     YuxColors.OPERATOR),
            AttributesDescriptor("构建注解",   YuxColors.METADATA),
            AttributesDescriptor("标识符",     YuxColors.IDENTIFIER),
            AttributesDescriptor("变量",       YuxColors.VARIABLE),
            AttributesDescriptor("参数",       YuxColors.PARAMETER),
            AttributesDescriptor("结构体字段", YuxColors.PROPERTY),
            AttributesDescriptor("结构体/类型", YuxColors.CLASS),
            AttributesDescriptor("函数声明",    YuxColors.FUNCTION_DECLARATION),
            AttributesDescriptor("函数调用",    YuxColors.FUNCTION_CALL),
            AttributesDescriptor("方法 / 构造", YuxColors.METHOD),
        )
    }
}
