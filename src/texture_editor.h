#pragma once
// texture_editor.h — Visual UV rect editor for PSB texture atlas entries.
// Shows the texture atlas image with draggable UV rectangles.

#include "forkparticle/psb/psb_reader.h"

#include <QDialog>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QImage>
#include <QMouseEvent>

namespace psb_editor {

// Widget that displays a texture atlas and lets the user drag UV rectangles.
class AtlasWidget : public QWidget {
    Q_OBJECT
public:
    explicit AtlasWidget(QWidget* parent = nullptr);

    void setImage(const QImage& img);
    void setUVRects(const std::vector<lu::assets::PsbFile::UVRect>& rects);
    void setSelectedIndex(int idx);
    int selectedIndex() const { return selectedIdx_; }

    lu::assets::PsbFile::UVRect uvRect(int idx) const;

signals:
    void uvChanged(int index, lu::assets::PsbFile::UVRect rect);
    void selectionChanged(int index);

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;

private:
    QPointF widgetToUV(QPoint pos) const;
    QPoint uvToWidget(float u, float v) const;
    QRect uvRectToWidget(const lu::assets::PsbFile::UVRect& r) const;
    int hitTest(QPoint pos) const;

    QImage image_;
    QRect imageRect_; // where the image is drawn in widget coords
    std::vector<lu::assets::PsbFile::UVRect> rects_;
    int selectedIdx_ = -1;

    // Drag state
    enum DragMode { None, Move, ResizeBR };
    DragMode dragMode_ = None;
    int dragIdx_ = -1;
    QPointF dragStartUV_;
    lu::assets::PsbFile::UVRect dragStartRect_;
};

// Dialog for editing PSB texture entries.
class TextureEditorDialog : public QDialog {
    Q_OBJECT
public:
    TextureEditorDialog(lu::assets::PsbFile& psb,
                         const QString& clientRoot,
                         QWidget* parent = nullptr);

    // Returns true if textures were modified.
    bool wasModified() const { return modified_; }

private slots:
    void onTextureSelected(int row);
    void onAddTexture();
    void onRemoveTexture();
    void onChangeTexturePath();
    void onUVChanged(int index, lu::assets::PsbFile::UVRect rect);

private:
    void refreshList();
    void loadAtlasImage();

    lu::assets::PsbFile& psb_;
    QString clientRoot_;
    bool modified_ = false;

    QListWidget* list_ = nullptr;
    AtlasWidget* atlas_ = nullptr;
    QLabel* infoLabel_ = nullptr;
};

} // namespace psb_editor
