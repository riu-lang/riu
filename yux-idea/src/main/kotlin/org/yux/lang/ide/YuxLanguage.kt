package org.yux.lang.ide

import com.intellij.lang.Language

object YuxLanguage : Language("yux") {
    private fun readResolve(): Any = YuxLanguage
}
