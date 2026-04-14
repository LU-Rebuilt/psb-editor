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

// One emitter instance with its own PSB, particles, and texture
struct EmitterInstance {
    lu::assets::PsbFile psb;
    std::vector<uint8_t> originalData;
    float transform[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    std::vector<Particle> particles;
    float emitAccum = 0.0f;
    float startDelay = 0.0f;   // TIME from effect file
    int trail = 0;              // TRAIL from effect file
    GLuint textureId = 0;
    bool hasTexture = false;
};

class ParticleGLWidget : public QOpenGLWidget {
    Q_OBJECT
public:
    explicit ParticleGLWidget(QWidget* parent = nullptr);

    // Load a single emitter (PSB file)
    void loadEmitter(const lu::assets::PsbFile& psb);

    // Load a full effect (.txt + all PSBs)
    void loadEffect(const lu::assets::EffectFile& effect, const QString& effectDir);

    void setClientRoot(const QString& root);
    void setPlaying(bool playing);
    bool isPlaying() const { return playing_; }
    void setPlaybackSpeed(float speed) { playbackSpeed_ = speed; }
    float playbackSpeed() const { return playbackSpeed_; }
    void setEmissionScale(float scale) { emissionScale_ = scale; }
    float emissionScale() const { return emissionScale_; }
    void setForceLoop(bool loop) { forceLoop_ = loop; }
    bool forceLoop() const { return forceLoop_; }

    // Access the primary emitter (first one / single PSB mode)
    lu::assets::PsbFile& primaryPsb() { return emitters_.empty() ? dummyPsb_ : emitters_[0].psb; }
    int emitterCount() const { return static_cast<int>(emitters_.size()); }

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
    QPoint lastMouse_;

    // Playback
    float playbackSpeed_ = 1.0f;
    float emissionScale_ = 1.0f;
    bool forceLoop_ = true;

    // Textures
    QString clientRoot_;
};

} // namespace psb_editor
