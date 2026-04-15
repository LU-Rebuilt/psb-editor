#pragma once

#include "forkparticle/psb/psb_reader.h"
#include "forkparticle/psb/psb_simulator.h"
#include "forkparticle/effect/effect_reader.h"
#include "microsoft/dds/dds_reader.h"

#include <QOpenGLWidget>
#include <QTimer>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QWheelEvent>

#include <vector>
#include <random>

namespace psb_editor {

using Particle = lu::assets::PsbParticle;

// Trail history entry for a single particle
struct TrailPoint {
    float x, y, z;
    float r, g, b, a;
    float size;
};

// One emitter instance with its own PSB, particles, and texture
struct EmitterInstance {
    lu::assets::PsbFile psb;
    std::vector<uint8_t> originalData;
    float transform[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    float baseTransform[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; // original from effect file
    float prevTransform[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; // previous frame for motion delta
    std::vector<Particle> particles;
    float emitAccum = 0.0f;
    float startDelay = 0.0f;   // TIME from effect file

    // Effect file flags
    int facing = 0;             // FACING: 0=camera, 1=worldX, 2=worldY, 3=worldZ, 4=emitter, 5=radial
    int trail = 0;              // TRAIL: draw connected geometry between positions
    int loop = 0;               // LOOP: override loop behavior
    int rot = 0;                // ROT: particles rotate with emitter
    int ds = 0;                 // DS: distance sort (back-to-front)
    int se = 0;                 // SE: scale by emitter
    int mt = 0;                 // MT: motion transform (local space)
    float dist = 0.0f;          // DIST: max render distance
    float dmin = 0.0f;          // DMIN: min render distance

    // Per-particle trail history (only when trail=1)
    // Maps particle index to ring buffer of recent positions
    std::vector<std::vector<TrailPoint>> trailHistory;

    GLuint textureId = 0;
    bool hasTexture = false;
};

class ParticleGLWidget : public QOpenGLWidget {
    Q_OBJECT
public:
    explicit ParticleGLWidget(QWidget* parent = nullptr);

    // Load a single emitter (PSB file)
    void loadEmitter(const lu::assets::PsbFile& psb);

    // Load a full effect (.txt + all PSBs). Set resetCamera=false on restart.
    void loadEffect(const lu::assets::EffectFile& effect, const QString& effectDir,
                     bool resetCamera = true);

    void setClientRoot(const QString& root);
    void setPlaying(bool playing);
    bool isPlaying() const { return playing_; }
    void setPlaybackSpeed(float speed) { playbackSpeed_ = speed; }
    float playbackSpeed() const { return playbackSpeed_; }
    void setEmissionScale(float scale) { emissionScale_ = scale; }
    float emissionScale() const { return emissionScale_; }
    void setForceLoop(bool loop) { forceLoop_ = loop; }
    bool forceLoop() const { return forceLoop_; }
    void setShowGrid(bool show) { showGrid_ = show; update(); }
    bool showGrid() const { return showGrid_; }
    void setEmitterMotion(bool enable) { emitterMotion_ = enable; }
    bool emitterMotion() const { return emitterMotion_; }

    // Frame all emitters in view (auto-zoom/pan to fit)
    void frameAllEmitters();

    // Access the primary emitter (first one / single PSB mode)
    lu::assets::PsbFile& primaryPsb() { return emitters_.empty() ? dummyPsb_ : emitters_[0].psb; }
    int emitterCount() const { return static_cast<int>(emitters_.size()); }

    // Access a specific emitter's PSB by index
    lu::assets::PsbFile& emitterPsb(int index) {
        if (index >= 0 && index < static_cast<int>(emitters_.size()))
            return emitters_[index].psb;
        return dummyPsb_;
    }

    // Reload a specific emitter's texture after texture edits
    void reloadEmitterTexture(int index) {
        if (index >= 0 && index < static_cast<int>(emitters_.size()))
            loadTextureForEmitter(emitters_[index]);
    }

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;

private:
    void tick();
    void loadTextureForEmitter(EmitterInstance& em);
    void clearEmitters();

    std::vector<EmitterInstance> emitters_;
    lu::assets::PsbFile dummyPsb_; // fallback for empty state

    bool loaded_ = false;
    bool playing_ = false;
    std::mt19937 rng_{42};

    // Timing
    QTimer tickTimer_;
    QElapsedTimer elapsed_;
    float totalTime_ = 0.0f;

    // Camera
    float orbitYaw_ = 30.0f;
    float orbitPitch_ = -20.0f;
    float zoom_ = 5.0f;
    float panX_ = 0.0f, panY_ = 0.0f, panZ_ = 0.0f;
    QPoint lastMouse_;

    // Playback
    float playbackSpeed_ = 1.0f;
    float emissionScale_ = 1.0f;
    bool forceLoop_ = true;
    bool showGrid_ = true;
    bool emitterMotion_ = false;

    // Textures
    QString clientRoot_;
};

} // namespace psb_editor
