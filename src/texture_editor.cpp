#include "texture_editor.h"
#include "file_browser.h"
#include "microsoft/dds/dds_reader.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QPainter>
#include <QFileInfo>
#include <QFile>
#include <QMessageBox>
#include <QInputDialog>

#include <cstring>

namespace {

// Software DXT1 block decoder (4x4 pixels per block, 8 bytes)
void decodeDXT1Block(const uint8_t* src, uint8_t* dst, int dstStride) {
    uint16_t c0 = src[0] | (src[1] << 8);
    uint16_t c1 = src[2] | (src[3] << 8);

    uint8_t colors[4][4]; // RGBA
    // Decode 565 to RGBA
    auto decode565 = [](uint16_t c, uint8_t* out) {
        out[0] = static_cast<uint8_t>(((c >> 11) & 0x1F) * 255 / 31);
        out[1] = static_cast<uint8_t>(((c >> 5) & 0x3F) * 255 / 63);
        out[2] = static_cast<uint8_t>((c & 0x1F) * 255 / 31);
        out[3] = 255;
    };
    decode565(c0, colors[0]);
    decode565(c1, colors[1]);

    if (c0 > c1) {
        for (int i = 0; i < 3; i++) {
            colors[2][i] = static_cast<uint8_t>((2 * colors[0][i] + colors[1][i]) / 3);
            colors[3][i] = static_cast<uint8_t>((colors[0][i] + 2 * colors[1][i]) / 3);
        }
        colors[2][3] = colors[3][3] = 255;
    } else {
        for (int i = 0; i < 3; i++)
            colors[2][i] = static_cast<uint8_t>((colors[0][i] + colors[1][i]) / 2);
        colors[2][3] = 255;
        colors[3][0] = colors[3][1] = colors[3][2] = 0;
        colors[3][3] = 0; // transparent
    }

    uint32_t bits = src[4] | (src[5] << 8) | (src[6] << 16) | (src[7] << 24);
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int idx = bits & 3;
            bits >>= 2;
            uint8_t* pixel = dst + y * dstStride + x * 4;
            std::memcpy(pixel, colors[idx], 4);
        }
    }
}

// Software DXT5 block decoder (4x4 pixels per block, 16 bytes)
void decodeDXT5Block(const uint8_t* src, uint8_t* dst, int dstStride) {
    // Alpha block (8 bytes)
    uint8_t a0 = src[0], a1 = src[1];
    uint8_t alphas[8];
    alphas[0] = a0;
    alphas[1] = a1;
    if (a0 > a1) {
        for (int i = 1; i < 7; i++)
            alphas[i + 1] = static_cast<uint8_t>(((7 - i) * a0 + i * a1) / 7);
    } else {
        for (int i = 1; i < 5; i++)
            alphas[i + 1] = static_cast<uint8_t>(((5 - i) * a0 + i * a1) / 5);
        alphas[6] = 0;
        alphas[7] = 255;
    }

    // 48 bits of alpha indices (6 bytes at src+2)
    uint64_t alphaBits = 0;
    for (int i = 0; i < 6; i++)
        alphaBits |= static_cast<uint64_t>(src[2 + i]) << (8 * i);

    // Color block (8 bytes at src+8)
    decodeDXT1Block(src + 8, dst, dstStride);

    // Apply alpha
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int alphaIdx = static_cast<int>(alphaBits & 7);
            alphaBits >>= 3;
            dst[y * dstStride + x * 4 + 3] = alphas[alphaIdx];
        }
    }
}

// Decode a DXT compressed DDS to QImage
QImage decodeDDS(const uint8_t* data, size_t dataSize, const lu::assets::DdsFile& dds) {
    QImage img(static_cast<int>(dds.width), static_cast<int>(dds.height),
               QImage::Format_RGBA8888);
    img.fill(Qt::transparent);

    const uint8_t* pixels = data + dds.data_offset;
    size_t pixelSize = dataSize - dds.data_offset;

    bool isDXT1 = (dds.four_cc == lu::assets::FOURCC_DXT1);
    bool isDXT5 = (dds.four_cc == lu::assets::FOURCC_DXT5);
    bool isDXT3 = (dds.four_cc == lu::assets::FOURCC_DXT3);

    if (!isDXT1 && !isDXT5 && !isDXT3) return img;

    int blockSize = isDXT1 ? 8 : 16;
    int blocksX = (static_cast<int>(dds.width) + 3) / 4;
    int blocksY = (static_cast<int>(dds.height) + 3) / 4;

    for (int by = 0; by < blocksY; by++) {
        for (int bx = 0; bx < blocksX; bx++) {
            size_t blockOff = static_cast<size_t>((by * blocksX + bx) * blockSize);
            if (blockOff + blockSize > pixelSize) break;

            // Decode to temp 4x4 RGBA block
            uint8_t block[4 * 4 * 4] = {};
            if (isDXT1) {
                decodeDXT1Block(pixels + blockOff, block, 16);
            } else if (isDXT5) {
                decodeDXT5Block(pixels + blockOff, block, 16);
            } else if (isDXT3) {
                // DXT3: explicit alpha (8 bytes) + DXT1 color block
                decodeDXT1Block(pixels + blockOff + 8, block, 16);
                // Apply explicit 4-bit alpha
                for (int y = 0; y < 4; y++) {
                    uint16_t row = pixels[blockOff + y * 2] | (pixels[blockOff + y * 2 + 1] << 8);
                    for (int x = 0; x < 4; x++) {
                        int a4 = (row >> (x * 4)) & 0xF;
                        block[y * 16 + x * 4 + 3] = static_cast<uint8_t>(a4 * 17);
                    }
                }
            }

            // Copy to QImage
            int px = bx * 4, py = by * 4;
            for (int y = 0; y < 4 && py + y < static_cast<int>(dds.height); y++) {
                uint8_t* scanline = img.scanLine(py + y);
                for (int x = 0; x < 4 && px + x < static_cast<int>(dds.width); x++) {
                    std::memcpy(scanline + (px + x) * 4, block + y * 16 + x * 4, 4);
                }
            }
        }
    }

    return img;
}

} // anonymous namespace

namespace psb_editor {

// ── AtlasWidget ──────────────────────────────────────────────────────────────

AtlasWidget::AtlasWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(256, 256);
    setMouseTracking(true);
}

void AtlasWidget::setImage(const QImage& img) {
    image_ = img;
    update();
}

void AtlasWidget::setUVRects(const std::vector<lu::assets::PsbFile::UVRect>& rects) {
    rects_ = rects;
    if (selectedIdx_ >= static_cast<int>(rects_.size()))
        selectedIdx_ = -1;
    update();
}

void AtlasWidget::setSelectedIndex(int idx) {
    selectedIdx_ = idx;
    update();
}

lu::assets::PsbFile::UVRect AtlasWidget::uvRect(int idx) const {
    if (idx >= 0 && idx < static_cast<int>(rects_.size()))
        return rects_[idx];
    return {0, 0, 1, 1};
}

QPointF AtlasWidget::widgetToUV(QPoint pos) const {
    if (imageRect_.width() <= 0 || imageRect_.height() <= 0) return {0, 0};
    float u = static_cast<float>(pos.x() - imageRect_.x()) / imageRect_.width();
    float v = static_cast<float>(pos.y() - imageRect_.y()) / imageRect_.height();
    return {std::clamp(u, 0.0f, 1.0f), std::clamp(v, 0.0f, 1.0f)};
}

QPoint AtlasWidget::uvToWidget(float u, float v) const {
    return {imageRect_.x() + static_cast<int>(u * imageRect_.width()),
            imageRect_.y() + static_cast<int>(v * imageRect_.height())};
}

QRect AtlasWidget::uvRectToWidget(const lu::assets::PsbFile::UVRect& r) const {
    QPoint tl = uvToWidget(r.u_min, r.v_min);
    QPoint br = uvToWidget(r.u_max, r.v_max);
    return QRect(tl, br);
}

int AtlasWidget::hitTest(QPoint pos) const {
    // Check if pos hits any UV rect (prefer selected, then first hit)
    if (selectedIdx_ >= 0 && selectedIdx_ < static_cast<int>(rects_.size())) {
        if (uvRectToWidget(rects_[selectedIdx_]).contains(pos))
            return selectedIdx_;
    }
    for (int i = static_cast<int>(rects_.size()) - 1; i >= 0; --i) {
        if (uvRectToWidget(rects_[i]).contains(pos))
            return i;
    }
    return -1;
}

void AtlasWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(40, 40, 45));

    if (image_.isNull()) {
        p.setPen(Qt::gray);
        p.drawText(rect(), Qt::AlignCenter, "No texture loaded\nSet client root first");
        return;
    }

    // Fit image in widget preserving aspect ratio
    QSize imgSize = image_.size().scaled(size(), Qt::KeepAspectRatio);
    imageRect_ = QRect(
        (width() - imgSize.width()) / 2,
        (height() - imgSize.height()) / 2,
        imgSize.width(), imgSize.height());

    p.drawImage(imageRect_, image_);

    // Draw UV rectangles
    for (int i = 0; i < static_cast<int>(rects_.size()); ++i) {
        QRect wr = uvRectToWidget(rects_[i]);
        bool selected = (i == selectedIdx_);

        // Fill with semi-transparent color
        QColor fill = selected ? QColor(255, 200, 50, 60) : QColor(100, 200, 255, 40);
        p.fillRect(wr, fill);

        // Border
        QPen pen(selected ? QColor(255, 200, 50) : QColor(100, 200, 255), selected ? 2 : 1);
        p.setPen(pen);
        p.drawRect(wr);

        // Label
        p.setPen(Qt::white);
        p.drawText(wr.topLeft() + QPoint(3, 12), QString::number(i));

        // Resize handle (bottom-right corner)
        if (selected) {
            QRect handle(wr.right() - 5, wr.bottom() - 5, 10, 10);
            p.fillRect(handle, QColor(255, 200, 50));
        }
    }
}

void AtlasWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;

    int hit = hitTest(e->pos());

    // Check resize handle first
    if (selectedIdx_ >= 0 && selectedIdx_ < static_cast<int>(rects_.size())) {
        QRect wr = uvRectToWidget(rects_[selectedIdx_]);
        QRect handle(wr.right() - 8, wr.bottom() - 8, 16, 16);
        if (handle.contains(e->pos())) {
            dragMode_ = ResizeBR;
            dragIdx_ = selectedIdx_;
            dragStartUV_ = widgetToUV(e->pos());
            dragStartRect_ = rects_[selectedIdx_];
            return;
        }
    }

    if (hit >= 0) {
        selectedIdx_ = hit;
        dragMode_ = Move;
        dragIdx_ = hit;
        dragStartUV_ = widgetToUV(e->pos());
        dragStartRect_ = rects_[hit];
        emit selectionChanged(hit);
    } else {
        selectedIdx_ = -1;
        emit selectionChanged(-1);
    }
    update();
}

void AtlasWidget::mouseMoveEvent(QMouseEvent* e) {
    if (dragMode_ == None || dragIdx_ < 0) return;

    QPointF uv = widgetToUV(e->pos());
    float du = static_cast<float>(uv.x() - dragStartUV_.x());
    float dv = static_cast<float>(uv.y() - dragStartUV_.y());

    auto& r = rects_[dragIdx_];

    if (dragMode_ == Move) {
        float w = dragStartRect_.u_max - dragStartRect_.u_min;
        float h = dragStartRect_.v_max - dragStartRect_.v_min;
        r.u_min = std::clamp(dragStartRect_.u_min + du, 0.0f, 1.0f - w);
        r.v_min = std::clamp(dragStartRect_.v_min + dv, 0.0f, 1.0f - h);
        r.u_max = r.u_min + w;
        r.v_max = r.v_min + h;
    } else if (dragMode_ == ResizeBR) {
        r.u_max = std::clamp(dragStartRect_.u_max + du, r.u_min + 0.01f, 1.01f);
        r.v_max = std::clamp(dragStartRect_.v_max + dv, r.v_min + 0.01f, 1.01f);
    }

    emit uvChanged(dragIdx_, r);
    update();
}

void AtlasWidget::mouseReleaseEvent(QMouseEvent*) {
    dragMode_ = None;
    dragIdx_ = -1;
}

// ── TextureEditorDialog ──────────────────────────────────────────────────────

TextureEditorDialog::TextureEditorDialog(lu::assets::PsbFile& psb,
                                           const QString& clientRoot,
                                           QWidget* parent)
    : QDialog(parent), psb_(psb), clientRoot_(clientRoot)
{
    setWindowTitle("Texture Editor");
    resize(800, 600);

    auto* mainLayout = new QHBoxLayout(this);

    // Left: texture list + buttons
    auto* leftPanel = new QVBoxLayout;
    list_ = new QListWidget;
    list_->setAlternatingRowColors(true);
    leftPanel->addWidget(new QLabel("Textures:"));
    leftPanel->addWidget(list_);

    auto* btnLayout = new QHBoxLayout;
    auto* addBtn = new QPushButton("Add");
    auto* removeBtn = new QPushButton("Remove");
    auto* changeBtn = new QPushButton("Change Path...");
    btnLayout->addWidget(addBtn);
    btnLayout->addWidget(removeBtn);
    btnLayout->addWidget(changeBtn);
    leftPanel->addLayout(btnLayout);

    infoLabel_ = new QLabel;
    leftPanel->addWidget(infoLabel_);

    auto* leftWidget = new QWidget;
    leftWidget->setLayout(leftPanel);
    leftWidget->setMaximumWidth(280);

    // Right: atlas viewer
    atlas_ = new AtlasWidget;

    auto* splitter = new QSplitter(Qt::Horizontal);
    splitter->addWidget(leftWidget);
    splitter->addWidget(atlas_);
    splitter->setStretchFactor(1, 1);
    mainLayout->addWidget(splitter);

    // Connections
    connect(list_, &QListWidget::currentRowChanged, this, &TextureEditorDialog::onTextureSelected);
    connect(addBtn, &QPushButton::clicked, this, &TextureEditorDialog::onAddTexture);
    connect(removeBtn, &QPushButton::clicked, this, &TextureEditorDialog::onRemoveTexture);
    connect(changeBtn, &QPushButton::clicked, this, &TextureEditorDialog::onChangeTexturePath);
    connect(atlas_, &AtlasWidget::uvChanged, this, &TextureEditorDialog::onUVChanged);
    connect(atlas_, &AtlasWidget::selectionChanged, [this](int idx) {
        if (idx >= 0 && idx < list_->count())
            list_->setCurrentRow(idx);
    });

    refreshList();
    loadAtlasImage();
}

void TextureEditorDialog::refreshList() {
    list_->clear();
    for (size_t i = 0; i < psb_.textures.size(); ++i) {
        QString label = QString("[%1] %2").arg(i).arg(QString::fromStdString(psb_.textures[i]));
        if (i < psb_.texture_uv_rects.size()) {
            const auto& uv = psb_.texture_uv_rects[i];
            label += QString(" (%1,%2)-(%3,%4)")
                .arg(static_cast<double>(uv.u_min), 0, 'f', 3)
                .arg(static_cast<double>(uv.v_min), 0, 'f', 3)
                .arg(static_cast<double>(uv.u_max), 0, 'f', 3)
                .arg(static_cast<double>(uv.v_max), 0, 'f', 3);
        }
        list_->addItem(label);
    }
    atlas_->setUVRects(psb_.texture_uv_rects);
}

void TextureEditorDialog::loadAtlasImage() {
    if (psb_.textures.empty()) return;

    QString texName = QString::fromStdString(psb_.textures[0]);
    texName.replace('\\', '/');
    QString baseName = QFileInfo(texName).completeBaseName();

    // Match the GL widget's path resolution logic
    QStringList tryPaths;
    QStringList roots = { clientRoot_ };

    for (const auto& root : roots) {
        if (root.isEmpty()) continue;
        tryPaths << root + "/res/forkp/textures/dds/" + baseName + ".dds";
        tryPaths << root + "/forkp/textures/dds/" + baseName + ".dds";
        tryPaths << root + "/res/" + texName;
        tryPaths << root + "/" + texName;
        // .tga -> .dds swap
        if (texName.endsWith(".tga", Qt::CaseInsensitive)) {
            QString ddsName = texName.left(texName.size() - 4) + ".dds";
            tryPaths << root + "/res/" + ddsName;
            tryPaths << root + "/" + ddsName;
        }
    }

    for (const auto& path : tryPaths) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) continue;

        QByteArray raw = f.readAll();
        auto data = reinterpret_cast<const uint8_t*>(raw.constData());
        if (raw.size() < 4 || data[0] != 'D' || data[1] != 'D' || data[2] != 'S') continue;

        try {
            auto dds = lu::assets::dds_parse_header(
                std::span<const uint8_t>(data, static_cast<size_t>(raw.size())));

            QImage img;
            if (dds.is_compressed) {
                img = decodeDDS(data, static_cast<size_t>(raw.size()), dds);
            } else {
                // Uncompressed BGRA
                img = QImage(static_cast<int>(dds.width), static_cast<int>(dds.height),
                             QImage::Format_RGBA8888);
                img.fill(QColor(60, 60, 65));
                const uint8_t* pixels = data + dds.data_offset;
                size_t pixelSize = raw.size() - dds.data_offset;
                if (pixelSize >= dds.width * dds.height * 4) {
                    for (uint32_t y = 0; y < dds.height; ++y) {
                        for (uint32_t x = 0; x < dds.width; ++x) {
                            size_t off = (y * dds.width + x) * 4;
                            uint8_t* scanline = img.scanLine(static_cast<int>(y));
                            scanline[x * 4 + 0] = pixels[off + 2]; // R
                            scanline[x * 4 + 1] = pixels[off + 1]; // G
                            scanline[x * 4 + 2] = pixels[off + 0]; // B
                            scanline[x * 4 + 3] = pixels[off + 3]; // A
                        }
                    }
                }
            }

            atlas_->setImage(img);
            infoLabel_->setText(QString("%1x%2 %3")
                .arg(dds.width).arg(dds.height)
                .arg(dds.is_compressed ? "DXT" : "RGBA"));
        } catch (...) {}
        break;
    }
}

void TextureEditorDialog::onTextureSelected(int row) {
    atlas_->setSelectedIndex(row);
}

void TextureEditorDialog::onAddTexture() {
    // Add a new texture entry with default UV covering a small region
    QString path = psb_.textures.empty() ? "texture_page_two.tga"
                                          : QString::fromStdString(psb_.textures[0]);

    bool ok;
    path = QInputDialog::getText(this, "Add Texture", "Texture filename:",
                                  QLineEdit::Normal, path, &ok);
    if (!ok || path.isEmpty()) return;

    psb_.textures.push_back(path.toStdString());
    psb_.texture_uv_rects.push_back({0.0f, 0.0f, 0.1f, 0.1f});
    psb_.num_textures = static_cast<uint32_t>(psb_.textures.size());
    modified_ = true;
    refreshList();
    list_->setCurrentRow(static_cast<int>(psb_.textures.size()) - 1);
}

void TextureEditorDialog::onRemoveTexture() {
    int row = list_->currentRow();
    if (row < 0 || row >= static_cast<int>(psb_.textures.size())) return;
    if (psb_.textures.size() <= 1) {
        QMessageBox::information(this, "Cannot Remove", "Must have at least one texture.");
        return;
    }

    psb_.textures.erase(psb_.textures.begin() + row);
    if (row < static_cast<int>(psb_.texture_uv_rects.size()))
        psb_.texture_uv_rects.erase(psb_.texture_uv_rects.begin() + row);
    psb_.num_textures = static_cast<uint32_t>(psb_.textures.size());
    modified_ = true;
    refreshList();
}

void TextureEditorDialog::onChangeTexturePath() {
    int row = list_->currentRow();
    if (row < 0 || row >= static_cast<int>(psb_.textures.size())) return;

    bool ok;
    QString current = QString::fromStdString(psb_.textures[row]);
    QString path = QInputDialog::getText(this, "Change Texture Path",
                                          "Texture filename:", QLineEdit::Normal,
                                          current, &ok);
    if (!ok || path.isEmpty()) return;

    psb_.textures[row] = path.toStdString();
    modified_ = true;
    refreshList();
    loadAtlasImage();
}

void TextureEditorDialog::onUVChanged(int index, lu::assets::PsbFile::UVRect rect) {
    if (index < 0 || index >= static_cast<int>(psb_.texture_uv_rects.size())) return;
    psb_.texture_uv_rects[index] = rect;
    modified_ = true;

    // Update list label
    if (index < list_->count()) {
        QString label = QString("[%1] %2 (%3,%4)-(%5,%6)")
            .arg(index)
            .arg(QString::fromStdString(psb_.textures[index]))
            .arg(static_cast<double>(rect.u_min), 0, 'f', 3)
            .arg(static_cast<double>(rect.v_min), 0, 'f', 3)
            .arg(static_cast<double>(rect.u_max), 0, 'f', 3)
            .arg(static_cast<double>(rect.v_max), 0, 'f', 3);
        list_->item(index)->setText(label);
    }

    infoLabel_->setText(QString("UV[%1]: (%2, %3) - (%4, %5)")
        .arg(index)
        .arg(static_cast<double>(rect.u_min), 0, 'f', 3)
        .arg(static_cast<double>(rect.v_min), 0, 'f', 3)
        .arg(static_cast<double>(rect.u_max), 0, 'f', 3)
        .arg(static_cast<double>(rect.v_max), 0, 'f', 3));
}

} // namespace psb_editor
