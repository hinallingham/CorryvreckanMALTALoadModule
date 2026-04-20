# SPDX-License-Identifier: MIT

# 1. 全体を管理するグローバルモジュールとして登録
# （この時点で MODULE_NAME という変数には "CorryvreckanModuleEventLoaderMALTA" という名前が自動で入る）
CORRYVRECKAN_GLOBAL_MODULE(MODULE_NAME)

# 2. コンパイルするソースファイルを指定
CORRYVRECKAN_MODULE_SOURCES(${MODULE_NAME}
    EventLoaderMALTA.cpp
)

# 3. ROOT::Tree をリンクする
# （プレフィックスを付けず、そのまま ${MODULE_NAME} を使うのが正解！）
target_link_libraries(${MODULE_NAME} ROOT::Tree)

# 4. インストール設定
CORRYVRECKAN_MODULE_INSTALL(${MODULE_NAME})