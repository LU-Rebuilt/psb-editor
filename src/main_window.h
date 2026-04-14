#pragma once

#include "forkparticle/psb/psb_reader.h"
#include "particle_gl_widget.h"

#include <QMainWindow>
#include <QLabel>
#include <QTreeWidget>
#include <QSlider>
#include <QUndoStack>

#include <functional>

namespace psb_editor {

class EditFieldCommand;
class EditTexturesCommand;

class MainWindow : public QMainWindow {
    Q_OBJECT
    friend class EditFieldCommand;
    friend class EditTexturesCommand;
public:
    explicit MainWindow(QWidget* parent = nullptr);

    bool openFile(const QString& path);

private slots:
    void onFileOpen();
    void onSave();
    void onSaveAs();
    void onSetClientRoot();
    void onTogglePlay();
    void onRestart();
    void onTreeItemChanged(QTreeWidgetItem* item, int column);
    void onTreeItemDoubleClicked(QTreeWidgetItem* item, int column);
    void onEditTextures();
    void onAddKeyframe();
    void onRemoveKeyframe();
    void onAddEmitter();
    void onRemoveEmitter();

private:
    void buildTree();
    void buildPsbTree(QTreeWidgetItem* parent, lu::assets::PsbFile& p);
    void addColor(QTreeWidgetItem* parent, const QString& name, lu::assets::PsbColor& c);
    void addFloat(QTreeWidgetItem* parent, const QString& name, float& ref, const QString& unit = {});
    void addUint(QTreeWidgetItem* parent, const QString& name, uint32_t& ref);
    void addHex(QTreeWidgetItem* parent, const QString& name, uint32_t& ref);
    void addReadonly(QTreeWidgetItem* parent, const QString& name, const QString& v);
    bool saveToFile(const QString& path);
    void applyChanges();
    void refreshTreeItem(QTreeWidgetItem* item);

    ParticleGLWidget* glWidget_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* speedLabel_ = nullptr;
    QLabel* emitLabel_ = nullptr;
    QUndoStack* undoStack_ = nullptr;

    lu::assets::PsbFile psb_;
    std::vector<uint8_t> originalData_;
    lu::assets::EffectFile effect_;
    QString effectDir_;
    bool isEffect_ = false;          // true when loaded from .txt
    QString currentPath_;
    bool loaded_ = false;
    bool buildingTree_ = false;

public:
    struct FieldBinding {
        enum Type { Float, Uint, Hex, ColorR, ColorG, ColorB, ColorA };
        Type type;
        void* ptr;
        QString unit;
    };

private:
    std::unordered_map<QTreeWidgetItem*, FieldBinding> bindings_;
    // Color swatch items → PsbColor* for color picker
    std::unordered_map<QTreeWidgetItem*, lu::assets::PsbColor*> colorItems_;
};

// Undo command for field edits
class EditFieldCommand : public QUndoCommand {
public:
    EditFieldCommand(MainWindow* window, QTreeWidgetItem* item,
                     MainWindow::FieldBinding binding,
                     float oldVal, float newVal);

    void undo() override;
    void redo() override;
    int id() const override { return 1; }
    bool mergeWith(const QUndoCommand* other) override;

private:
    MainWindow* window_;
    QTreeWidgetItem* item_;
    MainWindow::FieldBinding binding_;
    float oldVal_;
    float newVal_;
};

// Undo command for texture editor changes (add/remove/UV/path changes)
class EditTexturesCommand : public QUndoCommand {
public:
    EditTexturesCommand(MainWindow* window,
                         std::vector<std::string> oldTextures,
                         std::vector<lu::assets::PsbFile::UVRect> oldUVs,
                         uint32_t oldNumTex,
                         std::vector<std::string> newTextures,
                         std::vector<lu::assets::PsbFile::UVRect> newUVs,
                         uint32_t newNumTex);
    void undo() override;
    void redo() override;

private:
    void apply(const std::vector<std::string>& tex,
               const std::vector<lu::assets::PsbFile::UVRect>& uvs,
               uint32_t num);
    MainWindow* window_;
    std::vector<std::string> oldTex_, newTex_;
    std::vector<lu::assets::PsbFile::UVRect> oldUVs_, newUVs_;
    uint32_t oldNum_, newNum_;
};

} // namespace psb_editor
