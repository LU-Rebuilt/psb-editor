#include "particle_gl_widget.h"

#include <QOpenGLFunctions>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <cmath>
#include <algorithm>
#include <cstring>

using lu::assets::psb_spawn_particle;
using lu::assets::psb_tick_particles;
using lu::assets::psb_lerp_color;
using lu::assets::psb_lerp_size;
using lu::assets::psb_texture_index;
using lu::assets::PSB_DEG_TO_RAD;
static constexpr float DEG_TO_RAD = PSB_DEG_TO_RAD;

namespace psb_editor {

ParticleGLWidget::ParticleGLWidget(QWidget* parent)
    : QOpenGLWidget(parent)
{
    setMinimumSize(300, 300);
    setFocusPolicy(Qt::StrongFocus);
    connect(&tickTimer_, &QTimer::timeout, this, &ParticleGLWidget::tick);
}

void ParticleGLWidget::clearEmitters() {
    makeCurrent();
    for (auto& em : emitters_) {
        if (em.hasTexture) glDeleteTextures(1, &em.textureId);
    }
    doneCurrent();
    emitters_.clear();
}

void ParticleGLWidget::loadEmitter(const lu::assets::PsbFile& psb) {
    clearEmitters();
    EmitterInstance em;
    em.psb = psb;
    float id[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    std::memcpy(em.transform, id, sizeof(id));
    emitters_.push_back(std::move(em));
    loaded_ = true;
    totalTime_ = 0.0f;
    makeCurrent();
    loadTextureForEmitter(emitters_[0]);
    doneCurrent();
    elapsed_.restart();
    setPlaying(true);
    update();
}

void ParticleGLWidget::loadEffect(const lu::assets::EffectFile& effect,
                                    const QString& effectDir) {
    clearEmitters();
    for (const auto& ee : effect.emitters) {
        QString psbPath = effectDir + "/" + QString::fromStdString(ee.name) + ".psb";
        QFile f(psbPath);
        if (!f.open(QIODevice::ReadOnly)) continue;
        QByteArray raw = f.readAll();
        std::vector<uint8_t> data(raw.begin(), raw.end());
        EmitterInstance em;
        try {
            em.psb = lu::assets::psb_parse({data.data(), data.size()});
            em.originalData = std::move(data);
        } catch (...) { continue; }
        std::memcpy(em.transform, ee.transform, sizeof(ee.transform));
        em.startDelay = ee.time;
        em.trail = ee.trail;
        emitters_.push_back(std::move(em));
    }
    if (emitters_.empty()) return;
    loaded_ = true;
    totalTime_ = 0.0f;
    makeCurrent();
    for (auto& em : emitters_) loadTextureForEmitter(em);
    doneCurrent();
    elapsed_.restart();
    setPlaying(true);
    update();
}

void ParticleGLWidget::setClientRoot(const QString& root) {
    clientRoot_ = root;
    if (loaded_) {
        makeCurrent();
        for (auto& em : emitters_) loadTextureForEmitter(em);
        doneCurrent();
    }
}

void ParticleGLWidget::setPlaying(bool playing) {
    playing_ = playing;
    if (playing) { elapsed_.restart(); tickTimer_.start(16); }
    else tickTimer_.stop();
}

// ── Texture loading (unchanged logic) ────────────────────────────────────────

void ParticleGLWidget::loadTextureForEmitter(EmitterInstance& em) {
    if (em.hasTexture) { glDeleteTextures(1, &em.textureId); em.textureId = 0; em.hasTexture = false; }
    if (clientRoot_.isEmpty() || em.psb.textures.empty()) return;

    QString texName = QString::fromStdString(em.psb.textures[0]);
    texName.replace('\\', '/');
    QString baseName = QFileInfo(texName).completeBaseName();
    QStringList candidates = { texName };
    if (texName.endsWith(".tga", Qt::CaseInsensitive))
        candidates << texName.left(texName.size() - 4) + ".dds";
    candidates << "forkp/textures/dds/" + baseName + ".dds";

    QStringList tryPaths;
    for (const auto& cand : candidates) {
        tryPaths << clientRoot_ + "/res/" + cand;
        tryPaths << clientRoot_ + "/" + cand;
        tryPaths << clientRoot_ + "/client/res/" + cand;
    }

    for (const auto& path : tryPaths) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) continue;
        QByteArray raw = f.readAll();
        auto* data = reinterpret_cast<const uint8_t*>(raw.constData());
        if (raw.size() < 4 || data[0] != 'D' || data[1] != 'D' || data[2] != 'S') continue;
        try {
            auto dds = lu::assets::dds_parse_header({data, static_cast<size_t>(raw.size())});
            glGenTextures(1, &em.textureId);
            glBindTexture(GL_TEXTURE_2D, em.textureId);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            const uint8_t* pixels = data + dds.data_offset;
            size_t pixelSize = raw.size() - dds.data_offset;
            if (dds.is_compressed) {
                GLenum glFormat = 0;
                if (dds.four_cc == lu::assets::FOURCC_DXT1) glFormat = 0x83F1;
                else if (dds.four_cc == lu::assets::FOURCC_DXT3) glFormat = 0x83F2;
                else if (dds.four_cc == lu::assets::FOURCC_DXT5) glFormat = 0x83F3;
                if (glFormat) {
                    uint32_t bs = (glFormat == 0x83F1) ? 8 : 16;
                    uint32_t sz = ((dds.width+3)/4)*((dds.height+3)/4)*bs;
                    if (sz <= pixelSize) {
                        typedef void(*FN)(GLenum,GLint,GLenum,GLsizei,GLsizei,GLint,GLsizei,const void*);
                        auto fn = reinterpret_cast<FN>(context()->getProcAddress("glCompressedTexImage2D"));
                        if (fn) { fn(GL_TEXTURE_2D,0,glFormat,dds.width,dds.height,0,sz,pixels); em.hasTexture=true; }
                    }
                }
            } else if (pixelSize >= dds.width*dds.height*4) {
                glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,dds.width,dds.height,0,GL_BGRA,GL_UNSIGNED_BYTE,pixels);
                em.hasTexture = true;
            }
            glBindTexture(GL_TEXTURE_2D, 0);
        } catch (...) {}
        if (em.hasTexture) break;
    }
}

// ── Simulation (rewritten to match client 1:1) ──────────────────────────────

void ParticleGLWidget::tick() {
    if (!loaded_ || !playing_) return;
    float rawDt = elapsed_.restart() / 1000.0f;
    rawDt = std::min(rawDt, 0.05f);

    for (auto& em : emitters_) {
        const auto& psb = em.psb;
        float dt = rawDt * playbackSpeed_ * psb.playback_scale;
        totalTime_ += dt / static_cast<float>(emitters_.size());

        float delay = em.startDelay;
        if (totalTime_ < delay) continue;

        float rate = psb.birth_rate * emissionScale_;
        float period = std::abs(psb.emit_period);
        float lifetime = psb.life_max;
        float cycleDuration = period > 0 ? period : lifetime + psb.death_delay + 0.5f;
        float adjustedTime = totalTime_ - delay;
        float cycleTime = std::fmod(adjustedTime, cycleDuration);

        bool emitting;
        if (period > 0) emitting = true;
        else if (forceLoop_) emitting = (cycleTime < 0.2f);
        else emitting = (adjustedTime < 0.2f);

        if (rate > 0 && emitting) {
            em.emitAccum += rate * dt;
            while (em.emitAccum >= 1.0f) {
                em.particles.push_back(psb_spawn_particle(psb, em.transform, rng_));
                em.emitAccum -= 1.0f;
            }
        }

        psb_tick_particles(em.particles, psb, dt);
    }
    update();
}

// ── OpenGL ───────────────────────────────────────────────────────────────────

void ParticleGLWidget::initializeGL() {
    glClearColor(0.12f, 0.12f, 0.15f, 1.0f);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_POINT_SMOOTH);
}

void ParticleGLWidget::resizeGL(int w, int h) {
    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    float aspect = h > 0 ? static_cast<float>(w) / h : 1.0f;
    float fov = 45.0f * DEG_TO_RAD;
    float f = 1.0f / std::tan(fov * 0.5f);
    float n = 0.1f, fa = 1000.0f;
    GLfloat proj[16] = {};
    proj[0] = f/aspect; proj[5] = f;
    proj[10] = (fa+n)/(n-fa); proj[11] = -1.0f;
    proj[14] = 2.0f*fa*n/(n-fa);
    glLoadMatrixf(proj);
}

void ParticleGLWidget::paintGL() {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0, 0, -zoom_);
    glRotatef(-orbitPitch_, 1, 0, 0);
    glRotatef(-orbitYaw_, 0, 1, 0);

    // Grid + axes
    glDisable(GL_BLEND);
    glBegin(GL_LINES);
    glColor4f(0.3f, 0.3f, 0.3f, 0.5f);
    for (int i = -5; i <= 5; ++i) {
        glVertex3f(float(i),0,-5); glVertex3f(float(i),0,5);
        glVertex3f(-5,0,float(i)); glVertex3f(5,0,float(i));
    }
    glColor3f(1,0,0); glVertex3f(0,0,0); glVertex3f(1,0,0);
    glColor3f(0,1,0); glVertex3f(0,0,0); glVertex3f(0,1,0);
    glColor3f(0,0,1); glVertex3f(0,0,0); glVertex3f(0,0,1);
    glEnd();

    if (!loaded_) return;

    for (const auto& em : emitters_) {
        const auto& psb = em.psb;

        // Blend mode from FUN_01092380
        glEnable(GL_BLEND);
        switch (psb.texture_blend_mode) {
            case 1: glBlendFunc(GL_SRC_ALPHA, GL_ONE); break;
            case 2: glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR); break;
            case 3: glBlendFunc(GL_DST_COLOR, GL_ZERO); break;
            case 4: glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_COLOR); break;
            case 5: glBlendFunc(GL_SRC_ALPHA, GL_ONE); break;
            case 6: glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
            default: glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
        glDepthMask(GL_FALSE);

        if (em.hasTexture) {
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, em.textureId);
        }

        for (const auto& p : em.particles) {
            float t = 1.0f - (p.life / p.maxLife);
            auto c = psb_lerp_color(psb, t);
            float sz = psb_lerp_size(psb, t);
            float age = p.maxLife - p.life;
            int texIdx = psb_texture_index(psb, age);

            glPushMatrix();
            glTranslatef(p.x, p.y, p.z);
            // Billboard
            glRotatef(orbitYaw_, 0, 1, 0);
            glRotatef(orbitPitch_, 1, 0, 0);
            glRotatef(p.rotation / DEG_TO_RAD, 0, 0, 1); // back to degrees for GL

            glColor4f(c.r, c.g, c.b, c.a);
            glBegin(GL_QUADS);
            if (em.hasTexture && texIdx >= 0 && texIdx < static_cast<int>(psb.texture_uv_rects.size())) {
                float u0 = psb.texture_uv_rects[texIdx].u_min;
                float v0 = psb.texture_uv_rects[texIdx].v_min;
                float u1 = psb.texture_uv_rects[texIdx].u_max;
                float v1 = psb.texture_uv_rects[texIdx].v_max;
                glTexCoord2f(u0,v1); glVertex3f(-sz,-sz,0);
                glTexCoord2f(u1,v1); glVertex3f( sz,-sz,0);
                glTexCoord2f(u1,v0); glVertex3f( sz, sz,0);
                glTexCoord2f(u0,v0); glVertex3f(-sz, sz,0);
            } else {
                glVertex3f(-sz,-sz,0); glVertex3f(sz,-sz,0);
                glVertex3f(sz,sz,0); glVertex3f(-sz,sz,0);
            }
            glEnd();
            glPopMatrix();
        }

        if (em.hasTexture) {
            glBindTexture(GL_TEXTURE_2D, 0);
            glDisable(GL_TEXTURE_2D);
        }
        glDepthMask(GL_TRUE);
    }
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void ParticleGLWidget::mousePressEvent(QMouseEvent* e) { lastMouse_ = e->pos(); }
void ParticleGLWidget::mouseMoveEvent(QMouseEvent* e) {
    int dx = e->pos().x() - lastMouse_.x();
    int dy = e->pos().y() - lastMouse_.y();
    lastMouse_ = e->pos();
    if (e->buttons() & Qt::LeftButton) {
        orbitYaw_ += dx * 0.5f;
        orbitPitch_ += dy * 0.5f;
        orbitPitch_ = std::clamp(orbitPitch_, -89.0f, 89.0f);
        update();
    }
}
void ParticleGLWidget::wheelEvent(QWheelEvent* e) {
    zoom_ -= e->angleDelta().y() * 0.005f;
    zoom_ = std::clamp(zoom_, 0.5f, 100.0f);
    update();
}

} // namespace psb_editor
