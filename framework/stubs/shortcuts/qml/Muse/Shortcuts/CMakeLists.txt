muse_create_qml_module(muse_shortcuts_qml ALIAS muse::shortcuts_qml FOR muse_shortcuts STUB)

qt_add_qml_module(muse_shortcuts_qml
    URI Muse.Shortcuts
    VERSION 1.0
    QML_FILES
        Shortcuts.qml
    IMPORTS
        TARGET muse_ui_qml
        TARGET muse_uicomponents_qml
)

fixup_qml_module_dependencies(muse_shortcuts_qml)

target_link_libraries(muse_shortcuts_qml PRIVATE
    muse::ui_qml
    muse::uicomponents_qml
)
