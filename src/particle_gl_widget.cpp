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
                                    const QString& effectDir,
                                    bool resetCamera) {
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
        std::memcpy(em.baseTransform, ee.transform, sizeof(ee.transform));
        std::memcpy(em.prevTransform, ee.transform, sizeof(ee.transform));
        em.startDelay = ee.time;
        em.facing = ee.facing;
        em.trail = ee.trail;
        em.loop = ee.loop;
        em.rot = ee.rot;
        em.ds = ee.ds;
        em.se = ee.se;
        em.mt = ee.mt;
        em.dist = ee.dist;
        em.dmin = ee.dmin;
        emitters_.push_back(std::move(em));
    }
    if (emitters_.empty()) return;
    loaded_ = true;
    totalTime_ = 0.0f;
    makeCurrent();
    for (auto& em : emitters_) loadTextureForEmitter(em);
    doneCurrent();
    elapsed_.restart();
    // Auto-frame all emitters on first load only
    if (resetCamera) frameAllEmitters();
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

// ── Simulation (matches client: FUN_010d2f90 + FUN_0109bb30) ────────────────

void ParticleGLWidget::tick() {
    if (!loaded_ || !playing_) return;
    float rawDt = elapsed_.restart() / 1000.0f;
    rawDt = std::min(rawDt, 0.05f);

    for (auto& em : emitters_) {
        const auto& psb = em.psb;
        // Client: adjustedDt = dt * time_delta_mult (FUN_010d2f90)
        float dt = rawDt * playbackSpeed_ * psb.time_delta_mult;
        totalTime_ += dt / static_cast<float>(emitters_.size());

        // Save previous transform for motion delta (FUN_010d2f90: emitter+0xB0 = emitter+0xA0)
        std::memcpy(em.prevTransform, em.transform, sizeof(em.transform));

        // Emitter motion preview: orbit around base position
        if (emitterMotion_) {
            float orbitRadius = 3.0f;
            float orbitSpeed = 1.5f;
            float bobSpeed = 2.0f;
            float bobHeight = 1.0f;
            float angle = totalTime_ * orbitSpeed;

            // Orbit XZ + vertical bob
            em.transform[12] = em.baseTransform[12] + orbitRadius * std::cos(angle);
            em.transform[13] = em.baseTransform[13] + bobHeight * std::sin(totalTime_ * bobSpeed);
            em.transform[14] = em.baseTransform[14] + orbitRadius * std::sin(angle);

            // Rotate the emitter to face direction of travel (for ROT flag)
            float fwd_x = -std::sin(angle);
            float fwd_z = std::cos(angle);
            // Set rotation matrix columns (forward = Z, right = X, up = Y)
            em.transform[0] = fwd_z;  em.transform[1] = 0; em.transform[2] = -fwd_x;
            em.transform[4] = 0;      em.transform[5] = 1; em.transform[6] = 0;
            em.transform[8] = fwd_x;  em.transform[9] = 0; em.transform[10] = fwd_z;
        }

        float delay = em.startDelay;
        if (totalTime_ < delay) continue;

        float adjustedTime = totalTime_ - delay;

        // Emission rate with accumulator — matching FUN_010d2f90
        // Client: numToSpawn = emit_rate * adjustedDt, with fractional accumulator
        float rate = psb.emit_rate * emissionScale_;
        int maxParts = static_cast<int>(psb.max_particles);
        if (maxParts <= 0) maxParts = 10000;

        // Determine if we should be emitting
        float avgLife = psb.life_min + (psb.life_var - psb.life_min) * 0.5f;
        if (avgLife <= 0) avgLife = 1.0f;

        // LOOP flag from effect file overrides force-loop setting
        bool shouldLoop = forceLoop_ || (em.loop != 0);

        bool emitting;
        if (shouldLoop) {
            emitting = true;
        } else {
            emitting = (adjustedTime < avgLife);
        }

        if (rate > 0 && emitting) {
            float toSpawn = rate * dt;
            int intPart = static_cast<int>(toSpawn);
            em.emitAccum += (toSpawn - static_cast<float>(intPart));
            if (em.emitAccum >= 1.0f) {
                em.emitAccum -= 1.0f;
                intPart++;
            }
            // Cap at max_particles — FUN_010d2f90
            int currentCount = static_cast<int>(em.particles.size());
            if (currentCount + intPart > maxParts) {
                intPart = maxParts - currentCount;
            }
            for (int i = 0; i < intPart; ++i) {
                em.particles.push_back(psb_spawn_particle(psb, em.transform, rng_));
            }
        }

        psb_tick_particles(em.particles, psb, dt);

        // MT: motion transform — particles follow emitter movement
        // Client: FUN_01093fc0 checks flags & 0x20000, adds (currentPos - prevPos) to particles
        if (em.mt != 0 || em.rot != 0) {
            float dx = em.transform[12] - em.prevTransform[12];
            float dy = em.transform[13] - em.prevTransform[13];
            float dz = em.transform[14] - em.prevTransform[14];
            if (dx != 0 || dy != 0 || dz != 0) {
                for (auto& p : em.particles) {
                    p.x += dx;
                    p.y += dy;
                    p.z += dz;
                }
            }
        }

        // Record trail history for TRAIL emitters
        if (em.trail != 0) {
            // Resize trail history to match particle count
            em.trailHistory.resize(em.particles.size());
            for (size_t pi = 0; pi < em.particles.size(); ++pi) {
                const auto& p = em.particles[pi];
                float t = p.age / p.maxLife;
                auto c = psb_lerp_color(psb, t);
                float sz = psb_lerp_size(p, psb, t);
                em.trailHistory[pi].push_back({p.x, p.y, p.z, c.r, c.g, c.b, c.a, sz});
                // Keep max 32 trail points per particle
                if (em.trailHistory[pi].size() > 32)
                    em.trailHistory[pi].erase(em.trailHistory[pi].begin());
            }
        }
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
    glTranslatef(-panX_, -panY_, -panZ_);

    // Grid + axes
    if (showGrid_) {
        glDisable(GL_BLEND);
        int gridSize = std::max(5, static_cast<int>(zoom_ * 0.5f));
        glBegin(GL_LINES);
        glColor4f(0.3f, 0.3f, 0.3f, 0.5f);
        for (int i = -gridSize; i <= gridSize; ++i) {
            glVertex3f(float(i),0,float(-gridSize)); glVertex3f(float(i),0,float(gridSize));
            glVertex3f(float(-gridSize),0,float(i)); glVertex3f(float(gridSize),0,float(i));
        }
        glColor3f(1,0,0); glVertex3f(0,0,0); glVertex3f(1,0,0);
        glColor3f(0,1,0); glVertex3f(0,0,0); glVertex3f(0,1,0);
        glColor3f(0,0,1); glVertex3f(0,0,0); glVertex3f(0,0,1);
        glEnd();
    }

    if (!loaded_) return;

    // SE: extract emitter scale for particles that inherit it
    // DS: sort emitters by priority before rendering
    // Build render order (PRIO sorting)
    std::vector<size_t> renderOrder(emitters_.size());
    for (size_t i = 0; i < emitters_.size(); ++i) renderOrder[i] = i;
    std::sort(renderOrder.begin(), renderOrder.end(), [this](size_t a, size_t b) {
        // Lower priority renders first (background)
        return emitters_[a].psb.blend_mode < emitters_[b].psb.blend_mode;
    });

    // Camera position in world space (for distance culling)
    float camYawR = orbitYaw_ * DEG_TO_RAD;
    float camPitchR = orbitPitch_ * DEG_TO_RAD;
    float camX = panX_ + zoom_ * std::sin(camYawR) * std::cos(camPitchR);
    float camY = panY_ + zoom_ * std::sin(camPitchR);
    float camZ = panZ_ + zoom_ * std::cos(camYawR) * std::cos(camPitchR);

    for (size_t ei : renderOrder) {
        const auto& em = emitters_[ei];
        const auto& psb = em.psb;

        // DIST/DMIN culling — skip emitter if camera is outside distance range
        if (em.dist > 0.0f || em.dmin > 0.0f) {
            float dx = camX - em.transform[12];
            float dy = camY - em.transform[13];
            float dz = camZ - em.transform[14];
            float distSq = dx*dx + dy*dy + dz*dz;
            if (em.dist > 0.0f && distSq > em.dist * em.dist) continue;
            if (em.dmin > 0.0f && distSq < em.dmin * em.dmin) continue;
        }

        // SE: compute emitter scale factor from transform matrix
        float emitterScale = 1.0f;
        if (em.se != 0) {
            // Extract scale from first column of transform matrix
            float sx = std::sqrt(em.transform[0]*em.transform[0] +
                                 em.transform[1]*em.transform[1] +
                                 em.transform[2]*em.transform[2]);
            emitterScale = sx;
        }

        // Blend mode from FUN_01092380
        glEnable(GL_BLEND);
        switch (psb.blend_mode) {
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

        // DS: sort particles back-to-front by camera distance
        std::vector<size_t> particleOrder(em.particles.size());
        for (size_t i = 0; i < em.particles.size(); ++i) particleOrder[i] = i;
        if (em.ds != 0) {
            std::sort(particleOrder.begin(), particleOrder.end(),
                [&](size_t a, size_t b) {
                    const auto& pa = em.particles[a];
                    const auto& pb = em.particles[b];
                    float da = (pa.x-camX)*(pa.x-camX) + (pa.y-camY)*(pa.y-camY) + (pa.z-camZ)*(pa.z-camZ);
                    float db = (pb.x-camX)*(pb.x-camX) + (pb.y-camY)*(pb.y-camY) + (pb.z-camZ)*(pb.z-camZ);
                    return da > db; // far first
                });
        }

        // Decode particle rendering mode from PSB flags
        auto particleMode = lu::assets::decode_particle_mode(psb.flags);
        bool isVelocityAligned =
            (particleMode == lu::assets::ParticleMode::VelocityStreak ||
             particleMode == lu::assets::ParticleMode::VelocityStreakNoDrag);

        for (size_t pi : particleOrder) {
            const auto& p = em.particles[pi];
            float t = p.age / p.maxLife;
            auto c = psb_lerp_color(psb, t);
            float sz = psb_lerp_size(p, psb, t) * emitterScale; // SE: scale by emitter
            int texIdx = psb_texture_index(psb, p);

            float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
            if (em.hasTexture && texIdx >= 0 && texIdx < static_cast<int>(psb.texture_uv_rects.size())) {
                u0 = psb.texture_uv_rects[texIdx].u_min;
                v0 = psb.texture_uv_rects[texIdx].v_min;
                u1 = psb.texture_uv_rects[texIdx].u_max;
                v1 = psb.texture_uv_rects[texIdx].v_max;
            }

            glColor4f(c.r, c.g, c.b, c.a);

            if (isVelocityAligned) {
                // Velocity-aligned streak: quad stretches along velocity direction
                // The particle becomes a streak/spark oriented by its velocity
                float speed = std::sqrt(p.vx*p.vx + p.vy*p.vy + p.vz*p.vz);
                if (speed < 0.001f) {
                    // No velocity — fall back to billboard
                    glPushMatrix();
                    glTranslatef(p.x, p.y, p.z);
                    glRotatef(orbitYaw_, 0, 1, 0);
                    glRotatef(orbitPitch_, 1, 0, 0);
                    glBegin(GL_QUADS);
                    if (em.hasTexture) {
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
                } else {
                    // Build streak quad aligned to velocity, perpendicular to camera
                    float dirX = p.vx / speed, dirY = p.vy / speed, dirZ = p.vz / speed;

                    // Camera direction (view vector)
                    float cyR = orbitYaw_ * DEG_TO_RAD, cpR = orbitPitch_ * DEG_TO_RAD;
                    float viewX = std::sin(cyR) * std::cos(cpR);
                    float viewY = std::sin(cpR);
                    float viewZ = std::cos(cyR) * std::cos(cpR);

                    // Cross product: right = velocity × view (perpendicular to both)
                    float rx = dirY * viewZ - dirZ * viewY;
                    float ry = dirZ * viewX - dirX * viewZ;
                    float rz = dirX * viewY - dirY * viewX;
                    float rLen = std::sqrt(rx*rx + ry*ry + rz*rz);
                    if (rLen > 0.001f) { rx /= rLen; ry /= rLen; rz /= rLen; }

                    // Streak length proportional to speed, width = sz
                    float halfW = sz * 0.5f;
                    float halfL = sz + speed * 0.05f; // stretch by velocity

                    // 4 corners: along velocity ± halfL, perpendicular ± halfW
                    float x0 = p.x - dirX * halfL - rx * halfW;
                    float y0 = p.y - dirY * halfL - ry * halfW;
                    float z0 = p.z - dirZ * halfL - rz * halfW;
                    float x1 = p.x + dirX * halfL - rx * halfW;
                    float y1 = p.y + dirY * halfL - ry * halfW;
                    float z1 = p.z + dirZ * halfL - rz * halfW;
                    float x2 = p.x + dirX * halfL + rx * halfW;
                    float y2 = p.y + dirY * halfL + ry * halfW;
                    float z2 = p.z + dirZ * halfL + rz * halfW;
                    float x3 = p.x - dirX * halfL + rx * halfW;
                    float y3 = p.y - dirY * halfL + ry * halfW;
                    float z3 = p.z - dirZ * halfL + rz * halfW;

                    glBegin(GL_QUADS);
                    if (em.hasTexture) {
                        glTexCoord2f(u0,v1); glVertex3f(x0,y0,z0);
                        glTexCoord2f(u1,v1); glVertex3f(x1,y1,z1);
                        glTexCoord2f(u1,v0); glVertex3f(x2,y2,z2);
                        glTexCoord2f(u0,v0); glVertex3f(x3,y3,z3);
                    } else {
                        glVertex3f(x0,y0,z0); glVertex3f(x1,y1,z1);
                        glVertex3f(x2,y2,z2); glVertex3f(x3,y3,z3);
                    }
                    glEnd();
                }
            } else {
                // Billboard modes (0, 1, 3, 4, and model fallback 6-9)
                glPushMatrix();
                glTranslatef(p.x, p.y, p.z);

                // Facing mode (from effect FACING property)
                switch (em.facing) {
                    case 0:
                        glRotatef(orbitYaw_, 0, 1, 0);
                        glRotatef(orbitPitch_, 1, 0, 0);
                        break;
                    case 1: glRotatef(90.0f, 0, 1, 0); break;
                    case 2: glRotatef(90.0f, 1, 0, 0); break;
                    case 3: break;
                    case 4: {
                        float ex = em.transform[8], ey = em.transform[9], ez = em.transform[10];
                        float yaw = std::atan2(ex, ez) / DEG_TO_RAD;
                        float pitch = std::asin(std::clamp(-ey, -1.0f, 1.0f)) / DEG_TO_RAD;
                        glRotatef(yaw, 0, 1, 0);
                        glRotatef(pitch, 1, 0, 0);
                        break;
                    }
                    case 5: {
                        float ox = em.transform[12], oy = em.transform[13], oz = em.transform[14];
                        float rdx = p.x - ox, rdy = p.y - oy, rdz = p.z - oz;
                        float len = std::sqrt(rdx*rdx + rdy*rdy + rdz*rdz);
                        if (len > 0.001f) {
                            float yaw = std::atan2(rdx, rdz) / DEG_TO_RAD;
                            float pitch = std::asin(std::clamp(-rdy / len, -1.0f, 1.0f)) / DEG_TO_RAD;
                            glRotatef(yaw, 0, 1, 0);
                            glRotatef(pitch, 1, 0, 0);
                        } else {
                            glRotatef(orbitYaw_, 0, 1, 0);
                            glRotatef(orbitPitch_, 1, 0, 0);
                        }
                        break;
                    }
                    default:
                        glRotatef(orbitYaw_, 0, 1, 0);
                        glRotatef(orbitPitch_, 1, 0, 0);
                        break;
                }
                glRotatef(p.rotation / DEG_TO_RAD, 0, 0, 1);

                glBegin(GL_QUADS);
                if (em.hasTexture) {
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
        }

        if (em.hasTexture) {
            glBindTexture(GL_TEXTURE_2D, 0);
            glDisable(GL_TEXTURE_2D);
        }

        // Trail rendering — camera-facing ribbon strips with width from particle size
        if (em.trail != 0 && !em.trailHistory.empty()) {
            glDisable(GL_TEXTURE_2D);
            for (size_t pi = 0; pi < em.trailHistory.size(); ++pi) {
                const auto& trail = em.trailHistory[pi];
                if (trail.size() < 2) continue;
                glBegin(GL_QUAD_STRIP);
                for (size_t ti = 0; ti < trail.size(); ++ti) {
                    const auto& tp = trail[ti];
                    float trailFade = static_cast<float>(ti) / static_cast<float>(trail.size() - 1);
                    float w = tp.size * lu::assets::PSB_SIZE_SCALE * trailFade * emitterScale;
                    glColor4f(tp.r, tp.g, tp.b, tp.a * trailFade);
                    // Camera-facing ribbon: offset perpendicular to view direction
                    float upX = std::sin(orbitYaw_ * DEG_TO_RAD) * std::sin(orbitPitch_ * DEG_TO_RAD);
                    float upY = std::cos(orbitPitch_ * DEG_TO_RAD);
                    float upZ = std::cos(orbitYaw_ * DEG_TO_RAD) * std::sin(orbitPitch_ * DEG_TO_RAD);
                    // Simplified: use world Y as up for ribbon
                    glVertex3f(tp.x - w * 0.1f, tp.y + w, tp.z);
                    glVertex3f(tp.x + w * 0.1f, tp.y - w, tp.z);
                }
                glEnd();
            }
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
        // Orbit camera
        orbitYaw_ += dx * 0.5f;
        orbitPitch_ += dy * 0.5f;
        orbitPitch_ = std::clamp(orbitPitch_, -89.0f, 89.0f);
        update();
    } else if (e->buttons() & Qt::MiddleButton) {
        // Pan camera — move in screen-space plane
        float panSpeed = zoom_ * 0.003f;
        float yawRad = orbitYaw_ * DEG_TO_RAD;
        float pitchRad = orbitPitch_ * DEG_TO_RAD;

        // Right vector in world space
        float rx = std::cos(yawRad);
        float rz = -std::sin(yawRad);

        // Up vector in world space (simplified)
        float ux = std::sin(yawRad) * std::sin(pitchRad);
        float uy = std::cos(pitchRad);
        float uz = std::cos(yawRad) * std::sin(pitchRad);

        panX_ -= (rx * dx + ux * dy) * panSpeed;
        panY_ -= (         uy * dy) * panSpeed;
        panZ_ -= (rz * dx + uz * dy) * panSpeed;
        update();
    } else if (e->buttons() & Qt::RightButton) {
        // Zoom with right-drag (vertical)
        zoom_ -= dy * zoom_ * 0.005f;
        zoom_ = std::clamp(zoom_, 0.1f, 5000.0f);
        update();
    }
}
void ParticleGLWidget::wheelEvent(QWheelEvent* e) {
    // Smooth zoom — proportional to current distance
    float factor = 1.0f - e->angleDelta().y() * 0.001f;
    zoom_ *= factor;
    zoom_ = std::clamp(zoom_, 0.1f, 5000.0f);
    update();
}

void ParticleGLWidget::frameAllEmitters() {
    if (emitters_.empty()) return;

    // Compute bounding box of all emitter positions
    float minX = 1e9f, minY = 1e9f, minZ = 1e9f;
    float maxX = -1e9f, maxY = -1e9f, maxZ = -1e9f;
    for (const auto& em : emitters_) {
        float x = em.transform[12];
        float y = em.transform[13];
        float z = em.transform[14];
        minX = std::min(minX, x); maxX = std::max(maxX, x);
        minY = std::min(minY, y); maxY = std::max(maxY, y);
        minZ = std::min(minZ, z); maxZ = std::max(maxZ, z);
    }

    // Center pan on the midpoint
    panX_ = (minX + maxX) * 0.5f;
    panY_ = (minY + maxY) * 0.5f;
    panZ_ = (minZ + maxZ) * 0.5f;

    // Set zoom to fit the bounding box with some margin
    float extent = std::max({maxX - minX, maxY - minY, maxZ - minZ, 2.0f});
    zoom_ = extent * 2.0f;
    zoom_ = std::clamp(zoom_, 1.0f, 5000.0f);

    update();
}

} // namespace psb_editor
