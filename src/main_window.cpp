#include "main_window.h"
#include "texture_editor.h"
#include "file_browser.h"
#include "forkparticle/psb/psb_writer.h"
#include "forkparticle/effect/effect_reader.h"
#include "forkparticle/effect/effect_writer.h"

#include <QMenuBar>
#include <QStatusBar>
#include <QToolBar>
#include <QDockWidget>
#include <QFileDialog>
#include <QMessageBox>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSettings>
#include <QHeaderView>
#include <QColor>
#include <QColorDialog>
#include <QInputDialog>
#include <QBrush>
#include <QSlider>
#include <QCheckBox>

namespace psb_editor {

static const char* blendModeName(uint32_t mode) {
    switch (mode) {
        case 0: return "None";
        case 1: return "Additive";
        case 2: return "Screen";
        case 3: return "Multiply";
        case 4: return "Subtract";
        case 5: return "Additive (variant)";
        case 6: return "Alpha Blend";
        default: return "Unknown";
    }
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("PSB Particle Editor");
    resize(900, 700);

    // GL particle renderer as central widget
    glWidget_ = new ParticleGLWidget(this);
    setCentralWidget(glWidget_);

    // Property tree as dock
    auto* treeDock = new QDockWidget("Properties", this);
    treeDock->setObjectName("PsbProperties");
    tree_ = new QTreeWidget;
    tree_->setHeaderLabels({"Property", "Value"});
    tree_->setAlternatingRowColors(true);
    tree_->header()->setSectionResizeMode(QHeaderView::Interactive);
    tree_->setColumnWidth(0, 220);
    tree_->header()->setStretchLastSection(true);
    treeDock->setWidget(tree_);
    addDockWidget(Qt::RightDockWidgetArea, treeDock);

    // Undo stack
    undoStack_ = new QUndoStack(this);

    // Menu
    auto* fileMenu = menuBar()->addMenu("&File");
    fileMenu->addAction("&Open...", QKeySequence::Open,
                        this, &MainWindow::onFileOpen);
    fileMenu->addAction("&Save", QKeySequence::Save,
                        this, &MainWindow::onSave);
    fileMenu->addAction("Save &As...", QKeySequence("Ctrl+Shift+S"),
                        this, &MainWindow::onSaveAs);
    fileMenu->addSeparator();
    fileMenu->addAction("Set Client &Root...", QKeySequence("Ctrl+Shift+R"),
                        this, &MainWindow::onSetClientRoot);
    fileMenu->addSeparator();
    fileMenu->addAction("&Quit", QKeySequence::Quit,
                        this, &QMainWindow::close);

    auto* editMenu = menuBar()->addMenu("&Edit");
    auto* undoAction = undoStack_->createUndoAction(this, "&Undo");
    undoAction->setShortcut(QKeySequence::Undo);
    editMenu->addAction(undoAction);
    auto* redoAction = undoStack_->createRedoAction(this, "&Redo");
    redoAction->setShortcut(QKeySequence("Ctrl+Shift+Z"));
    editMenu->addAction(redoAction);

    editMenu->addSeparator();
    editMenu->addAction("Edit &Textures...", QKeySequence("Ctrl+T"),
                        this, &MainWindow::onEditTextures);
    editMenu->addSeparator();
    editMenu->addAction("Add &Keyframe", this, &MainWindow::onAddKeyframe);
    editMenu->addAction("Remove Last Keyframe", this, &MainWindow::onRemoveKeyframe);
    editMenu->addSeparator();
    editMenu->addAction("Add Emitter to Effect", this, &MainWindow::onAddEmitter);
    editMenu->addAction("Remove Last Emitter", this, &MainWindow::onRemoveEmitter);

    auto* viewMenu = menuBar()->addMenu("&Playback");
    viewMenu->addAction("&Play/Pause", QKeySequence(Qt::Key_Space),
                        this, &MainWindow::onTogglePlay);
    viewMenu->addAction("&Restart", QKeySequence("Ctrl+R"),
                        this, &MainWindow::onRestart);

    // Playback toolbar
    auto* toolbar = addToolBar("Playback");
    toolbar->setObjectName("PlaybackToolbar");

    toolbar->addWidget(new QLabel(" Speed: "));
    auto* speedSlider = new QSlider(Qt::Horizontal);
    speedSlider->setRange(1, 100);
    speedSlider->setValue(10);
    speedSlider->setFixedWidth(120);
    toolbar->addWidget(speedSlider);
    speedLabel_ = new QLabel(" 1.0x ");
    toolbar->addWidget(speedLabel_);

    toolbar->addSeparator();

    toolbar->addWidget(new QLabel(" Emission: "));
    auto* emitSlider = new QSlider(Qt::Horizontal);
    emitSlider->setRange(1, 200);
    emitSlider->setValue(10);
    emitSlider->setFixedWidth(120);
    toolbar->addWidget(emitSlider);
    emitLabel_ = new QLabel(" 1.0x ");
    toolbar->addWidget(emitLabel_);

    connect(speedSlider, &QSlider::valueChanged, [this](int val) {
        float speed = val / 10.0f;
        glWidget_->setPlaybackSpeed(speed);
        speedLabel_->setText(QString(" %1x ").arg(static_cast<double>(speed), 0, 'f', 1));
    });
    connect(emitSlider, &QSlider::valueChanged, [this](int val) {
        float scale = val / 10.0f;
        glWidget_->setEmissionScale(scale);
        emitLabel_->setText(QString(" %1x ").arg(static_cast<double>(scale), 0, 'f', 1));
    });

    toolbar->addSeparator();

    auto* loopCheck = new QCheckBox("Force Loop");
    loopCheck->setChecked(true);
    toolbar->addWidget(loopCheck);
    connect(loopCheck, &QCheckBox::toggled, [this](bool checked) {
        glWidget_->setForceLoop(checked);
    });

    auto* gridCheck = new QCheckBox("Grid");
    gridCheck->setChecked(true);
    toolbar->addWidget(gridCheck);
    connect(gridCheck, &QCheckBox::toggled, [this](bool checked) {
        glWidget_->setShowGrid(checked);
    });

    toolbar->addSeparator();

    auto* motionCheck = new QCheckBox("Emitter Motion");
    motionCheck->setChecked(false);
    motionCheck->setToolTip("Preview emitter movement (orbit + bob) to test ROT/MT/Trail flags");
    toolbar->addWidget(motionCheck);
    connect(motionCheck, &QCheckBox::toggled, [this](bool checked) {
        glWidget_->setEmitterMotion(checked);
    });

    // Help menu
    auto* helpMenu = menuBar()->addMenu("&Help");
    helpMenu->addAction("&Controls", [this]() {
        QMessageBox::information(this, "Controls",
            "Camera Controls:\n"
            "  Left drag: Orbit\n"
            "  Middle drag: Pan\n"
            "  Right drag: Zoom\n"
            "  Scroll wheel: Zoom\n\n"
            "Shortcuts:\n"
            "  Ctrl+O: Open PSB/Effect\n"
            "  Ctrl+S: Save\n"
            "  Ctrl+T: Edit Textures\n"
            "  Ctrl+R: Restart\n"
            "  Space: Play/Pause\n"
            "  Ctrl+Z / Ctrl+Shift+Z: Undo/Redo\n\n"
            "Toolbar Toggles:\n"
            "  Force Loop: Continuously re-emit particles\n"
            "  Grid: Show/hide ground grid and axes\n"
            "  Emitter Motion: Preview particles on a moving\n"
            "    emitter (orbit + bob) to test ROT/MT/Trail");
    });

    // Tree edit signals
    connect(tree_, &QTreeWidget::itemChanged, this, &MainWindow::onTreeItemChanged);
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, &MainWindow::onTreeItemDoubleClicked);

    // Status
    statusLabel_ = new QLabel("No file loaded");
    statusBar()->addPermanentWidget(statusLabel_);

    // Restore client root
    QSettings settings;
    QString savedRoot = settings.value("psb_client_root").toString();
    if (!savedRoot.isEmpty()) {
        glWidget_->setClientRoot(savedRoot);
    }
}

bool MainWindow::openFile(const QString& path) {
    // Auto-detect client root from file path
    QSettings settings;
    if (settings.value("psb_client_root").toString().isEmpty()) {
        QDir dir = QFileInfo(path).absoluteDir();
        for (int i = 0; i < 8; ++i) {
            if (QDir(dir.absoluteFilePath("forkp")).exists()) {
                glWidget_->setClientRoot(dir.absolutePath());
                settings.setValue("psb_client_root", dir.absolutePath());
                break;
            }
            if (dir.dirName() == "res") {
                glWidget_->setClientRoot(dir.absoluteFilePath(".."));
                settings.setValue("psb_client_root", dir.absoluteFilePath(".."));
                break;
            }
            if (!dir.cdUp()) break;
        }
    }

    // Handle .txt effect files — load full effect with all emitters
    if (path.endsWith(".txt", Qt::CaseInsensitive)) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QMessageBox::warning(this, "Error", QString("Could not open:\n%1").arg(path));
            return false;
        }
        QString text = file.readAll();
        auto effect = lu::assets::effect_parse(text.toStdString());
        if (effect.emitters.empty()) {
            QMessageBox::warning(this, "Error", "No emitters found in effect file.");
            return false;
        }

        currentPath_ = path;
        undoStack_->clear();
        setWindowTitle(QString("PSB Editor - %1 (%2 emitters)")
            .arg(QFileInfo(path).fileName()).arg(effect.emitters.size()));

        effect_ = effect;
        effectDir_ = QFileInfo(path).absolutePath();
        isEffect_ = true;

        glWidget_->loadEffect(effect_, effectDir_);

        // Load the first emitter's PSB as the editable primary
        psb_ = glWidget_->primaryPsb();
        loaded_ = true;
        buildTree();

        statusLabel_->setText(
            QString("Effect: %1 emitters").arg(effect_.emitters.size()));
        return true;
    }

    isEffect_ = false;

    // Handle .psb files — single emitter
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, "Error", QString("Could not open:\n%1").arg(path));
        return false;
    }

    QByteArray raw = file.readAll();
    originalData_.assign(raw.begin(), raw.end());

    try {
        psb_ = lu::assets::psb_parse(
            std::span<const uint8_t>(originalData_.data(), originalData_.size()));
        loaded_ = true;
    } catch (const std::exception& e) {
        QMessageBox::warning(this, "Error", QString("Failed to parse PSB:\n%1").arg(e.what()));
        return false;
    }

    currentPath_ = path;
    undoStack_->clear();
    setWindowTitle(QString("PSB Editor - %1").arg(QFileInfo(path).fileName()));

    glWidget_->loadEmitter(psb_);
    buildTree();

    QString texInfo = psb_.textures.empty() ? "no textures" :
        QString::fromStdString(psb_.textures[0]);
    statusLabel_->setText(
        QString("Particle %1 | %2 tex | %3 bytes | %4")
            .arg(psb_.particle_id).arg(psb_.num_assets)
            .arg(psb_.file_total_size).arg(texInfo));

    return true;
}

void MainWindow::onFileOpen() {
    QSettings settings;
    QString lastDir = settings.value("psb_last_open_dir").toString();
    QString path = qt_common::FileBrowserDialog::getOpenFileName(this,
        "Open PSB/Effect File", lastDir,
        "Particle Files (*.psb *.txt);;PSB Files (*.psb);;Effect Files (*.txt);;All Files (*)");
    if (!path.isEmpty()) {
        settings.setValue("psb_last_open_dir", QFileInfo(path).absolutePath());
        openFile(path);
    }
}

void MainWindow::onSave() {
    if (!loaded_ || currentPath_.isEmpty()) {
        onSaveAs();
        return;
    }
    if (saveToFile(currentPath_)) {
        statusBar()->showMessage(QString("Saved: %1").arg(currentPath_), 5000);
    }
}

void MainWindow::onSaveAs() {
    if (!loaded_) return;
    QString filter = isEffect_ ? "Effect Files (*.txt);;All Files (*)"
                                : "PSB Files (*.psb);;All Files (*)";
    QString suffix = isEffect_ ? ".txt" : ".psb";
    QString path = qt_common::FileBrowserDialog::getSaveFileName(this,
        isEffect_ ? "Save Effect File" : "Save PSB File", currentPath_, filter);
    if (!path.isEmpty()) {
        if (!path.contains('.')) path += suffix;
        if (saveToFile(path)) {
            currentPath_ = path;
            setWindowTitle(QString("PSB Editor - %1").arg(QFileInfo(path).fileName()));
            statusBar()->showMessage(QString("Saved: %1").arg(path), 5000);
        }
    }
}

bool MainWindow::saveToFile(const QString& path) {
    if (isEffect_) {
        // Write the effect .txt
        std::string text = lu::assets::effect_write(effect_);
        QFile out(path);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QMessageBox::warning(this, "Error", QString("Could not write:\n%1").arg(path));
            return false;
        }
        out.write(text.c_str(), static_cast<qint64>(text.size()));
        return true;
    }

    // Write PSB
    auto data = lu::assets::psb_write(psb_,
        std::span<const uint8_t>(originalData_.data(), originalData_.size()));
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, "Error", QString("Could not write:\n%1").arg(path));
        return false;
    }
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<qint64>(data.size()));
    return true;
}

void MainWindow::onSetClientRoot() {
    QSettings settings;
    QString current = settings.value("psb_client_root").toString();
    QString dir = QFileDialog::getExistingDirectory(
        this, "Select Client Root (containing res/)", current,
        QFileDialog::DontUseNativeDialog);
    if (!dir.isEmpty()) {
        settings.setValue("psb_client_root", dir);
        glWidget_->setClientRoot(dir);
        statusBar()->showMessage(QString("Client root: %1").arg(dir), 5000);
    }
}

void MainWindow::onEditTextures() {
    if (!loaded_) return;

    QSettings settings;
    QString root = settings.value("psb_client_root").toString();

    if (isEffect_ && glWidget_->emitterCount() > 1) {
        // Effect mode: let user pick which emitter to edit
        QStringList items;
        for (int i = 0; i < glWidget_->emitterCount(); ++i) {
            auto& ep = glWidget_->emitterPsb(i);
            QString label = QString("Emitter %1").arg(i);
            if (i < static_cast<int>(effect_.emitters.size()))
                label += QString(" (%1)").arg(QString::fromStdString(effect_.emitters[i].name));
            if (!ep.textures.empty())
                label += QString(" - %1 tex").arg(ep.textures.size());
            items << label;
        }

        bool ok;
        QString chosen = QInputDialog::getItem(this, "Edit Textures",
            "Select emitter:", items, 0, false, &ok);
        if (!ok) return;
        int emIdx = items.indexOf(chosen);
        if (emIdx < 0) return;

        auto& targetPsb = glWidget_->emitterPsb(emIdx);
        auto oldTex = targetPsb.textures;
        auto oldUVs = targetPsb.texture_uv_rects;
        auto oldNum = targetPsb.num_assets;

        TextureEditorDialog dlg(targetPsb, root, this);
        dlg.exec();

        if (dlg.wasModified()) {
            glWidget_->reloadEmitterTexture(emIdx);
            statusBar()->showMessage(
                QString("Textures updated for emitter %1").arg(emIdx), 5000);
        }
    } else {
        // Single PSB mode or single-emitter effect
        auto oldTex = psb_.textures;
        auto oldUVs = psb_.texture_uv_rects;
        auto oldNum = psb_.num_assets;

        TextureEditorDialog dlg(psb_, root, this);
        dlg.exec();

        if (dlg.wasModified()) {
            undoStack_->push(new EditTexturesCommand(
                this, oldTex, oldUVs, oldNum,
                psb_.textures, psb_.texture_uv_rects, psb_.num_assets));
        }
    }
}

void MainWindow::onAddKeyframe() {
    if (!loaded_) return;

    // Snapshot current state for undo
    auto oldKfs = psb_.anim_timeline.keyframes;
    auto oldOffsets = psb_.anim_timeline.frame_offsets;
    auto oldCount = psb_.anim_timeline.frame_count;

    // Create a new keyframe from current emitter state
    lu::assets::PsbFile::AnimKeyframe kf;
    kf.initial_color = psb_.initial_color;
    kf.trans_color_1 = psb_.trans_color_1;
    kf.trans_color_2 = psb_.trans_color_2;
    kf.final_color = psb_.final_color;
    kf.color_ratio_1 = psb_.color_ratio_1;
    kf.color_ratio_2 = psb_.color_ratio_2;
    kf.life_min = psb_.life_min;
    kf.life_var = psb_.life_var;
    kf.vel_min = psb_.vel_min;
    kf.vel_var = psb_.vel_var;
    kf.flags = psb_.flags;
    kf.initial_scale = psb_.initial_scale;
    kf.trans_scale = psb_.trans_scale;
    kf.final_scale = psb_.final_scale;
    kf.scale_ratio = psb_.scale_ratio;
    kf.rot_min = psb_.rot_min;
    kf.rot_var = psb_.rot_var;
    kf.drag = psb_.drag;
    kf.scale_x = psb_.scale[0];
    kf.scale_y = psb_.scale[1];
    kf.scale_z = psb_.scale[2];
    kf.scale_w = psb_.scale[3];
    kf.rotation_x = psb_.rotation[0];
    kf.tint = psb_.tint;

    psb_.anim_timeline.keyframes.push_back(std::move(kf));
    psb_.anim_timeline.frame_offsets.push_back(0); // placeholder offset
    psb_.anim_timeline.frame_count = static_cast<uint32_t>(psb_.anim_timeline.keyframes.size());

    glWidget_->loadEmitter(psb_);
    buildTree();
    statusBar()->showMessage(
        QString("Added keyframe %1").arg(psb_.anim_timeline.frame_count - 1), 5000);
}

void MainWindow::onRemoveKeyframe() {
    if (!loaded_ || psb_.anim_timeline.keyframes.empty()) return;

    psb_.anim_timeline.keyframes.pop_back();
    if (!psb_.anim_timeline.frame_offsets.empty())
        psb_.anim_timeline.frame_offsets.pop_back();
    psb_.anim_timeline.frame_count = static_cast<uint32_t>(psb_.anim_timeline.keyframes.size());

    glWidget_->loadEmitter(psb_);
    buildTree();
    statusBar()->showMessage(
        QString("Removed keyframe. %1 remaining").arg(psb_.anim_timeline.frame_count), 5000);
}

void MainWindow::onAddEmitter() {
    if (!loaded_ || !isEffect_) {
        QMessageBox::information(this, "Not an Effect",
            "Load an effect .txt file first to add emitters.");
        return;
    }

    bool ok;
    QString name = QInputDialog::getText(this, "Add Emitter", "Emitter name (PSB filename without .psb):",
                                          QLineEdit::Normal, "", &ok);
    if (!ok || name.isEmpty()) return;

    lu::assets::EffectEmitter ee;
    ee.name = name.toStdString();
    // Identity transform
    float id[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    std::memcpy(ee.transform, id, sizeof(id));

    effect_.emitters.push_back(std::move(ee));
    glWidget_->loadEffect(effect_, effectDir_);
    buildTree();
    statusBar()->showMessage(QString("Added emitter: %1").arg(name), 5000);
}

void MainWindow::onRemoveEmitter() {
    if (!loaded_ || !isEffect_ || effect_.emitters.empty()) return;

    effect_.emitters.pop_back();
    glWidget_->loadEffect(effect_, effectDir_);
    buildTree();
    statusBar()->showMessage(
        QString("Removed emitter. %1 remaining").arg(effect_.emitters.size()), 5000);
}

void MainWindow::onTogglePlay() {
    if (!loaded_) return;
    glWidget_->setPlaying(!glWidget_->isPlaying());
    statusBar()->showMessage(glWidget_->isPlaying() ? "Playing" : "Paused", 2000);
}

void MainWindow::onRestart() {
    if (!loaded_) return;
    if (isEffect_) {
        glWidget_->loadEffect(effect_, effectDir_, /*resetCamera=*/false);
    } else {
        glWidget_->loadEmitter(psb_);
    }
    statusBar()->showMessage("Restarted", 2000);
}

void MainWindow::applyChanges() {
    if (!loaded_) return;
    glWidget_->loadEmitter(psb_);
}

// ── Tree item editing ────────────────────────────────────────────────────────

void MainWindow::onTreeItemChanged(QTreeWidgetItem* item, int column) {
    if (buildingTree_ || column != 1) return;

    auto it = bindings_.find(item);
    if (it == bindings_.end()) return;

    const auto& binding = it->second;
    QString text = item->text(1).trimmed();
    if (!binding.unit.isEmpty() && text.endsWith(binding.unit))
        text = text.left(text.size() - binding.unit.size()).trimmed();

    bool ok = false;
    float newVal = 0;

    switch (binding.type) {
        case FieldBinding::Float:
        case FieldBinding::ColorR:
        case FieldBinding::ColorG:
        case FieldBinding::ColorB:
        case FieldBinding::ColorA:
            newVal = text.toFloat(&ok);
            break;
        case FieldBinding::Uint:
            newVal = static_cast<float>(text.toUInt(&ok));
            break;
        case FieldBinding::Hex:
            if (text.startsWith("0x", Qt::CaseInsensitive)) text = text.mid(2);
            newVal = static_cast<float>(text.toUInt(&ok, 16));
            if (!ok) newVal = static_cast<float>(text.toUInt(&ok));
            break;
    }
    if (!ok) return;

    float oldVal;
    if (binding.type == FieldBinding::Uint || binding.type == FieldBinding::Hex)
        oldVal = static_cast<float>(*static_cast<uint32_t*>(binding.ptr));
    else
        oldVal = *static_cast<float*>(binding.ptr);

    if (oldVal == newVal) return;

    undoStack_->push(new EditFieldCommand(this, item, binding, oldVal, newVal));
}

void MainWindow::refreshTreeItem(QTreeWidgetItem* item) {
    auto it = bindings_.find(item);
    if (it == bindings_.end()) return;

    const auto& binding = it->second;
    buildingTree_ = true; // suppress itemChanged signal

    switch (binding.type) {
        case FieldBinding::Float:
        case FieldBinding::ColorR:
        case FieldBinding::ColorG:
        case FieldBinding::ColorB:
        case FieldBinding::ColorA: {
            QString text = QString::number(static_cast<double>(*static_cast<float*>(binding.ptr)), 'f', 4);
            if (!binding.unit.isEmpty()) text += " " + binding.unit;
            item->setText(1, text);
            break;
        }
        case FieldBinding::Uint:
            item->setText(1, QString::number(*static_cast<uint32_t*>(binding.ptr)));
            break;
        case FieldBinding::Hex:
            item->setText(1, QString("0x%1").arg(*static_cast<uint32_t*>(binding.ptr), 8, 16, QChar('0')));
            break;
    }

    buildingTree_ = false;
}

// ── Undo command ─────────────────────────────────────────────────────────────

EditFieldCommand::EditFieldCommand(MainWindow* window, QTreeWidgetItem* item,
                                     MainWindow::FieldBinding binding,
                                     float oldVal, float newVal)
    : QUndoCommand(QString("Edit %1").arg(item->text(0)))
    , window_(window), item_(item), binding_(binding)
    , oldVal_(oldVal), newVal_(newVal)
{}

void EditFieldCommand::undo() {
    if (binding_.type == MainWindow::FieldBinding::Uint ||
        binding_.type == MainWindow::FieldBinding::Hex)
        *static_cast<uint32_t*>(binding_.ptr) = static_cast<uint32_t>(oldVal_);
    else
        *static_cast<float*>(binding_.ptr) = oldVal_;
    window_->refreshTreeItem(item_);
    window_->applyChanges();
}

void EditFieldCommand::redo() {
    if (binding_.type == MainWindow::FieldBinding::Uint ||
        binding_.type == MainWindow::FieldBinding::Hex)
        *static_cast<uint32_t*>(binding_.ptr) = static_cast<uint32_t>(newVal_);
    else
        *static_cast<float*>(binding_.ptr) = newVal_;
    window_->refreshTreeItem(item_);
    window_->applyChanges();
}

bool EditFieldCommand::mergeWith(const QUndoCommand* other) {
    auto* cmd = static_cast<const EditFieldCommand*>(other);
    if (cmd->item_ != item_ || cmd->binding_.ptr != binding_.ptr) return false;
    newVal_ = cmd->newVal_;
    return true;
}

// ── Tree building ────────────────────────────────────────────────────────────

void MainWindow::addFloat(QTreeWidgetItem* parent, const QString& name,
                           float& ref, const QString& unit) {
    auto* item = new QTreeWidgetItem(parent);
    item->setFlags(item->flags() | Qt::ItemIsEditable);
    item->setText(0, name);
    QString text = QString::number(static_cast<double>(ref), 'f', 4);
    if (!unit.isEmpty()) text += " " + unit;
    item->setText(1, text);
    bindings_[item] = {FieldBinding::Float, &ref, unit};
}

void MainWindow::addUint(QTreeWidgetItem* parent, const QString& name, uint32_t& ref) {
    auto* item = new QTreeWidgetItem(parent);
    item->setFlags(item->flags() | Qt::ItemIsEditable);
    item->setText(0, name);
    item->setText(1, QString::number(ref));
    bindings_[item] = {FieldBinding::Uint, &ref, {}};
}

void MainWindow::addHex(QTreeWidgetItem* parent, const QString& name, uint32_t& ref) {
    auto* item = new QTreeWidgetItem(parent);
    item->setFlags(item->flags() | Qt::ItemIsEditable);
    item->setText(0, name);
    item->setText(1, QString("0x%1").arg(ref, 8, 16, QChar('0')));
    bindings_[item] = {FieldBinding::Hex, &ref, {}};
}

void MainWindow::addReadonly(QTreeWidgetItem* parent, const QString& name, const QString& v) {
    auto* item = new QTreeWidgetItem(parent);
    item->setText(0, name);
    item->setText(1, v);
}

static void updateColorSwatch(QTreeWidgetItem* item, const lu::assets::PsbColor& c) {
    item->setText(1, QString("(%1, %2, %3, %4)")
        .arg(static_cast<double>(c.r), 0, 'f', 3)
        .arg(static_cast<double>(c.g), 0, 'f', 3)
        .arg(static_cast<double>(c.b), 0, 'f', 3)
        .arg(static_cast<double>(c.a), 0, 'f', 3));

    int r = std::clamp(static_cast<int>(c.r * 255), 0, 255);
    int g = std::clamp(static_cast<int>(c.g * 255), 0, 255);
    int b = std::clamp(static_cast<int>(c.b * 255), 0, 255);
    item->setBackground(1, QBrush(QColor(r, g, b)));
    int lum = (r * 299 + g * 587 + b * 114) / 1000;
    item->setForeground(1, QBrush(lum < 128 ? Qt::white : Qt::black));
}

void MainWindow::addColor(QTreeWidgetItem* parent, const QString& name,
                           lu::assets::PsbColor& c) {
    auto* item = new QTreeWidgetItem(parent);
    item->setText(0, name);
    updateColorSwatch(item, c);
    colorItems_[item] = &c;

    // Add editable sub-items for each component
    addFloat(item, "R", c.r);
    addFloat(item, "G", c.g);
    addFloat(item, "B", c.b);
    addFloat(item, "A", c.a);
}

void MainWindow::onTreeItemDoubleClicked(QTreeWidgetItem* item, int column) {
    if (!item || !loaded_) return;

    // Color swatch — open color picker
    auto it = colorItems_.find(item);
    if (it == colorItems_.end()) return;

    lu::assets::PsbColor* c = it->second;
    QColor initial(
        std::clamp(static_cast<int>(c->r * 255), 0, 255),
        std::clamp(static_cast<int>(c->g * 255), 0, 255),
        std::clamp(static_cast<int>(c->b * 255), 0, 255),
        std::clamp(static_cast<int>(c->a * 255), 0, 255));

    QColorDialog dlg(initial, this);
    dlg.setOption(QColorDialog::ShowAlphaChannel, true);
    if (dlg.exec() != QDialog::Accepted) return;

    QColor chosen = dlg.currentColor();
    lu::assets::PsbColor oldColor = *c;
    lu::assets::PsbColor newColor = {
        chosen.redF(), chosen.greenF(), chosen.blueF(), chosen.alphaF()
    };

    // Push as 4 individual undo commands grouped via macro
    undoStack_->beginMacro(QString("Edit %1").arg(item->text(0)));
    // Find the R/G/B/A child items
    for (int i = 0; i < item->childCount(); ++i) {
        auto* child = item->child(i);
        auto bit = bindings_.find(child);
        if (bit == bindings_.end()) continue;
        float oldVal = *static_cast<float*>(bit->second.ptr);
        float newVal;
        if (child->text(0) == "R") newVal = newColor.r;
        else if (child->text(0) == "G") newVal = newColor.g;
        else if (child->text(0) == "B") newVal = newColor.b;
        else if (child->text(0) == "A") newVal = newColor.a;
        else continue;
        if (oldVal != newVal)
            undoStack_->push(new EditFieldCommand(this, child, bit->second, oldVal, newVal));
    }
    undoStack_->endMacro();

    // Update swatch
    buildingTree_ = true;
    updateColorSwatch(item, *c);
    buildingTree_ = false;
}

void MainWindow::buildTree() {
    buildingTree_ = true;
    tree_->clear();
    bindings_.clear();
    colorItems_.clear();
    if (!loaded_) { buildingTree_ = false; return; }

    if (isEffect_ && !effect_.emitters.empty()) {
        // Effect mode: each emitter gets its own subtree
        static const char* facingNames[] = {
            "Camera Billboard", "World X", "World Y", "World Z", "Emitter", "Radial"
        };

        for (size_t i = 0; i < effect_.emitters.size(); ++i) {
            auto& ee = effect_.emitters[i];
            auto* emRoot = new QTreeWidgetItem(tree_);
            emRoot->setText(0, QString("Emitter %1: %2").arg(i).arg(QString::fromStdString(ee.name)));
            emRoot->setExpanded(true);

            // Effect properties for this emitter
            auto* props = new QTreeWidgetItem(emRoot);
            props->setText(0, "Effect Properties");
            props->setExpanded(true);
            addFloat(props, "Position X", ee.transform[12]);
            addFloat(props, "Position Y", ee.transform[13]);
            addFloat(props, "Position Z", ee.transform[14]);
            if (ee.time > 0)
                addReadonly(props, "Start Delay (TIME)", QString::number(static_cast<double>(ee.time), 'f', 3) + " s");
            addFloat(props, "Max Distance (DIST)", ee.dist);
            addFloat(props, "Min Distance (DMIN)", ee.dmin);

            // Facing mode with human-readable name
            const char* faceName = (ee.facing >= 0 && ee.facing <= 5) ? facingNames[ee.facing] : "Unknown";
            addReadonly(props, "Facing (FACING)", QString("%1 (%2)").arg(faceName).arg(ee.facing));

            // Boolean flags — show with descriptions
            addReadonly(props, "Rotate (ROT)", ee.rot ? "Yes — particles rotate with emitter" : "No");
            addReadonly(props, "Trail (TRAIL)", ee.trail ? "Yes — connected trail geometry" : "No");
            addReadonly(props, "Distance Sort (DS)", ee.ds ? "Yes — back-to-front sort" : "No");
            addReadonly(props, "Scale by Emitter (SE)", ee.se ? "Yes — inherit emitter scale" : "No");
            addReadonly(props, "Motion Transform (MT)", ee.mt ? "Yes — local-space movement" : "No");
            addReadonly(props, "Loop (LOOP)", ee.loop ? "Yes — emitter loops" : "No");
            addReadonly(props, "Priority (PRIO)", QString::number(ee.prio));

            // PSB particle data for this emitter
            if (i < static_cast<size_t>(glWidget_->emitterCount())) {
                buildPsbTree(emRoot, glWidget_->emitterPsb(static_cast<int>(i)));
            }
        }
    } else {
        // Single PSB mode
        buildPsbTree(nullptr, psb_);
    }

    tree_->expandToDepth(1);
    buildingTree_ = false;
}

void MainWindow::buildPsbTree(QTreeWidgetItem* parent, lu::assets::PsbFile& p) {
    // If parent is null, add directly to tree root; otherwise add as children
    auto addItem = [&](const QString& label) -> QTreeWidgetItem* {
        if (parent) return new QTreeWidgetItem(parent);
        return new QTreeWidgetItem(tree_);
    };

    // Helper: add section item to parent or tree root
    auto makeSection = [&](const QString& label) -> QTreeWidgetItem* {
        auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree_);
        item->setText(0, label);
        return item;
    };

    // Header (read-only)
    auto* hdr = makeSection("Header");
    hdr->setText(0, "Header");
    hdr->setExpanded(true);
    addReadonly(hdr, "Particle ID", QString::number(p.particle_id));
    addReadonly(hdr, "Header Size", QString::number(p.header_size));
    addReadonly(hdr, "Data Size", QString::number(p.data_size));
    addReadonly(hdr, "Section Offset", QString::number(p.section_offset));
    addReadonly(hdr, "File Size", QString::number(p.file_total_size));
    addReadonly(hdr, "Params Size", QString::number(p.emitter_params_size));
    if (!p.emitter_name.empty())
        addReadonly(hdr, "Emitter Name", QString::fromStdString(p.emitter_name));
    addHex(hdr, "Flags (*PFLAGS)", p.flags);
    // Decode and show particle rendering mode from flags
    {
        auto mode = lu::assets::decode_particle_mode(p.flags);
        static const char* modeNames[] = {
            "Billboard", "Billboard (neg rot)", "Velocity Streak",
            "Billboard Alt", "Billboard (neg rot 2)", "Velocity Streak (no drag)",
            "3D Model", "3D Model (2)", "3D Model (neg rot)", "3D Model (4)"
        };
        int mi = static_cast<int>(mode);
        QString modeStr = (mi >= 0 && mi <= 9) ? modeNames[mi] : "Unknown";
        addReadonly(hdr, "Particle Mode", QString("%1 (%2)").arg(modeStr).arg(mi));
        if (p.flags & 0x100) addReadonly(hdr, "Skip Drag/Forces", "Yes (flags & 0x100)");
        if (p.flags & 0x200000) addReadonly(hdr, "Spin Direction", "Always positive (flags & 0x200000)");
        if (p.flags & 0x400000) addReadonly(hdr, "Spin Direction", "Always negative (flags & 0x400000)");
    }

    // Colors — official ForkParticle names from MediaFX
    auto* colors = makeSection("Colors");
    colors->setExpanded(true);
    addColor(colors, "Initial Color (*PICOLOR)", p.initial_color);
    addColor(colors, "Transitional Color 1 (*PTCOLOR1)", p.trans_color_1);
    addColor(colors, "Transitional Color 2 (*PTCOLOR2)", p.trans_color_2);
    addColor(colors, "Final Color (*PFCOLOR)", p.final_color);
    addColor(colors, "Tint (*TINT)", p.tint);
    addFloat(colors, "Color Ratio 1 (*PCOLORRATIO)", p.color_ratio_1);
    addFloat(colors, "Color Ratio 2 (*PCOLORRATIO2)", p.color_ratio_2);

    // Particle Properties
    auto* particle = makeSection("Particle Properties");
    particle->setExpanded(true);
    addFloat(particle, "Life Min (*PLIFEMIN)", p.life_min, "s");
    addFloat(particle, "Life Variance (*PLIFEVAR)", p.life_var, "s");
    addFloat(particle, "Velocity Min (*PVELMIN)", p.vel_min, "u/s");
    addFloat(particle, "Velocity Variance (*PVELVAR)", p.vel_var, "u/s");

    // Scale
    auto* size = makeSection("Scale");
    size->setExpanded(true);
    addFloat(size, "Initial Scale (*PISCALE)", p.initial_scale);
    addFloat(size, "Transitional Scale (*PTSCALE)", p.trans_scale);
    addFloat(size, "Final Scale (*PFSCALE)", p.final_scale);
    addFloat(size, "Scale Ratio (*PSCALERATIO)", p.scale_ratio);
    addFloat(size, "Scale X (*SCALE[0])", p.scale[0]);
    addFloat(size, "Scale Y (*SCALE[1])", p.scale[1]);
    addFloat(size, "Scale Z (*SCALE[2])", p.scale[2]);
    addFloat(size, "Scale W (*SCALE[3])", p.scale[3]);
    addFloat(size, "IScale Variation (*ISCALEMIN)", p.iscale_var);
    addFloat(size, "TScale Variation (*TSCALEMIN)", p.tscale_var);
    addFloat(size, "FScale Variation (*FSCALEMIN)", p.fscale_var);

    // Rotation & Drag
    auto* rot = makeSection("Rotation / Drag");
    addFloat(rot, "Rotation Min (*PROTMIN)", p.rot_min, "deg");
    addFloat(rot, "Rotation Variance (*PROTVAR)", p.rot_var, "deg/s");
    addFloat(rot, "Drag (*PDRAG)", p.drag);
    addFloat(rot, "Rotation X (*ROTATION[0])", p.rotation[0], "deg");
    addFloat(rot, "Rotation Y (*ROTATION[1])", p.rotation[1], "deg");
    addFloat(rot, "Rotation Z (*ROTATION[2])", p.rotation[2], "deg");
    addFloat(rot, "Rotation W (*ROTATION[3])", p.rotation[3], "deg");

    // Emitter Properties
    auto* emitter = makeSection("Emitter Properties");
    emitter->setExpanded(true);
    addFloat(emitter, "Emit Rate (*ERATE)", p.emit_rate, "/s");
    addFloat(emitter, "Gravity (*EGRAVITY)", p.gravity, "u/s2");
    addFloat(emitter, "Cone Radius (*ECONERAD)", p.cone_radius, "deg");
    addFloat(emitter, "Max Particles (*EMAXPARTICLE)", p.max_particles);
    addFloat(emitter, "Plane W (*EPLANEW)", p.plane_w);
    addFloat(emitter, "Plane H (*EPLANEH)", p.plane_h);
    addFloat(emitter, "Plane D (*EPLANED)", p.plane_d);
    addUint(emitter, "Volume Type (*EVOLUME)", p.volume_type);
    addFloat(emitter, "Sim Life (*ESIMLIFE)", p.sim_life, "s");
    addFloat(emitter, "Emitter Life (*ELIFE)", p.emitter_life, "s");
    addFloat(emitter, "Burst Count (*NBURST)", p.num_burst);

    // Rendering
    auto* render = makeSection("Rendering");
    addReadonly(render, "Blend Mode",
        QString("%1 (%2)").arg(blendModeName(p.blend_mode)).arg(p.blend_mode));
    addUint(render, "Blend Mode ID (*EBLENDMODE)", p.blend_mode);
    addFloat(render, "Time Delta Mult (*TDELTAMULT)", p.time_delta_mult, "x");
    addFloat(render, "Anim Speed (*ANMSPEED)", p.anim_speed);
    addUint(render, "Point Forces (*NUMPOINTFORCES)", p.num_point_forces);
    addUint(render, "Emission Assets", p.num_emission_assets);

    // Bounds
    auto* bounds = makeSection("Bounds (AABB)");
    addFloat(bounds, "Min X", p.bounds_min[0]);
    addFloat(bounds, "Min Y", p.bounds_min[1]);
    addFloat(bounds, "Min Z", p.bounds_min[2]);
    addFloat(bounds, "Max X", p.bounds_max[0]);
    addFloat(bounds, "Max Y", p.bounds_max[1]);
    addFloat(bounds, "Max Z", p.bounds_max[2]);

    // Path Properties
    auto* path = makeSection("Path Properties");
    addFloat(path, "Path Distance Min", p.path_dist_min);
    addFloat(path, "Path Distance Var", p.path_dist_var);
    addFloat(path, "Path Speed", p.path_speed);

    // Emitter Offset
    auto* offset = makeSection("Emitter Offset (*OFFSET)");
    addFloat(offset, "Offset X", p.emitter_offset_x);
    addFloat(offset, "Offset Y", p.emitter_offset_y);
    addFloat(offset, "Offset Z", p.emitter_offset_z);

    // Textures — each entry is a sprite variant randomly assigned to particles
    auto* tex = makeSection(QString("Sprite Textures (%1)").arg(p.textures.size()));
    tex->setExpanded(true);
    addReadonly(tex, "Edit textures with Edit > Edit Textures (Ctrl+T)", "");
    for (size_t i = 0; i < p.textures.size(); ++i) {
        auto* texItem = new QTreeWidgetItem(tex);
        texItem->setText(0, QString("Sprite %1").arg(i));
        texItem->setText(1, QString::fromStdString(p.textures[i]));
        if (i < p.texture_uv_rects.size()) {
            const auto& uv = p.texture_uv_rects[i];
            addReadonly(texItem, "UV Rect",
                QString("(%1, %2) - (%3, %4)")
                    .arg(static_cast<double>(uv.u_min), 0, 'f', 3)
                    .arg(static_cast<double>(uv.v_min), 0, 'f', 3)
                    .arg(static_cast<double>(uv.u_max), 0, 'f', 3)
                    .arg(static_cast<double>(uv.v_max), 0, 'f', 3));
        }
    }

    // Animation timeline (if present)
    if (p.anim_timeline.frame_count > 0) {
        const auto& at = p.anim_timeline;
        auto* anim = makeSection(QString("Animation Timeline (%1 keyframes)").arg(at.frame_count));
        if (!at.name.empty())
            addReadonly(anim, "Name", QString::fromStdString(at.name));
        addReadonly(anim, "Framerate", QString::number(
            static_cast<double>(at.framerate), 'f', 1));
        addReadonly(anim, "Entry Size", QString::number(at.entry_size));

        for (size_t i = 0; i < at.keyframes.size(); ++i) {
            const auto& kf = at.keyframes[i];
            auto* kfItem = new QTreeWidgetItem(anim);
            kfItem->setText(0, QString("Keyframe %1").arg(i));
            if (i < at.frame_offsets.size())
                kfItem->setText(1, QString("offset %1").arg(at.frame_offsets[i]));

            // Show color values
            auto addKfColor = [&](const QString& name, const lu::assets::PsbColor& c) {
                addReadonly(kfItem, name,
                    QString("(%1, %2, %3, %4)")
                        .arg(static_cast<double>(c.r), 0, 'f', 3)
                        .arg(static_cast<double>(c.g), 0, 'f', 3)
                        .arg(static_cast<double>(c.b), 0, 'f', 3)
                        .arg(static_cast<double>(c.a), 0, 'f', 3));
            };
            addKfColor("Initial Color", kf.initial_color);
            addKfColor("Trans Color 1", kf.trans_color_1);
            addKfColor("Trans Color 2", kf.trans_color_2);
            addKfColor("Final Color", kf.final_color);
            addKfColor("Tint", kf.tint);

            // Show key parameter diffs from main block
            auto addKfDiff = [&](const QString& name, float kfVal, float mainVal) {
                if (std::abs(kfVal - mainVal) > 0.0001f) {
                    addReadonly(kfItem, name,
                        QString("%1 (main: %2)")
                            .arg(static_cast<double>(kfVal), 0, 'f', 4)
                            .arg(static_cast<double>(mainVal), 0, 'f', 4));
                }
            };
            addKfDiff("Life Min", kf.life_min, p.life_min);
            addKfDiff("Life Var", kf.life_var, p.life_var);
            addKfDiff("Vel Min", kf.vel_min, p.vel_min);
            addKfDiff("Initial Scale", kf.initial_scale, p.initial_scale);
            addKfDiff("Trans Scale", kf.trans_scale, p.trans_scale);
            addKfDiff("Final Scale", kf.final_scale, p.final_scale);
            addKfDiff("Rot Min", kf.rot_min, p.rot_min);
            addKfDiff("Rot Var", kf.rot_var, p.rot_var);
            addKfDiff("Drag", kf.drag, p.drag);
        }
    }

}

// ── EditTexturesCommand ──────────────────────────────────────────────────────

EditTexturesCommand::EditTexturesCommand(
    MainWindow* window,
    std::vector<std::string> oldTextures,
    std::vector<lu::assets::PsbFile::UVRect> oldUVs,
    uint32_t oldNumTex,
    std::vector<std::string> newTextures,
    std::vector<lu::assets::PsbFile::UVRect> newUVs,
    uint32_t newNumTex)
    : QUndoCommand("Edit Textures")
    , window_(window)
    , oldTex_(std::move(oldTextures)), newTex_(std::move(newTextures))
    , oldUVs_(std::move(oldUVs)), newUVs_(std::move(newUVs))
    , oldNum_(oldNumTex), newNum_(newNumTex)
{}

void EditTexturesCommand::undo() {
    apply(oldTex_, oldUVs_, oldNum_);
}

void EditTexturesCommand::redo() {
    apply(newTex_, newUVs_, newNum_);
}

void EditTexturesCommand::apply(
    const std::vector<std::string>& tex,
    const std::vector<lu::assets::PsbFile::UVRect>& uvs,
    uint32_t num) {
    window_->psb_.textures = tex;
    window_->psb_.texture_uv_rects = uvs;
    window_->psb_.num_assets = num;
    window_->glWidget_->loadEmitter(window_->psb_);
    window_->buildTree();
}

} // namespace psb_editor
