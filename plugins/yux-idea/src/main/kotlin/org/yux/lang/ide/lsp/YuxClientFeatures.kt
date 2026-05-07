package org.yux.lang.ide.lsp

import com.redhat.devtools.lsp4ij.client.features.LSPClientFeatures

class YuxClientFeatures : LSPClientFeatures() {
    init {
        semanticTokensFeature = YuxSemanticTokensFeature()
    }
}
