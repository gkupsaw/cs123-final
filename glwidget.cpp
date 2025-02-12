#include "glwidget.h"
#include <QMouseEvent>
#include <sstream>

#include "shapes/Island.h"
#include "shapes/RoundedCylinder.h"
#include "shapes/Leaf.h"
#include "shapes/sphere.h"
#include "shapes/Cone.h"
#include "shapes/cube.h"
#include "camera/orbitingcamera.h"
#include "lib/resourceloader.h"
#include "uniforms/varsfile.h"
#include "gl/shaders/shaderattriblocations.h"
#include "glm/gtc/type_ptr.hpp"
#include "glm/gtc/matrix_transform.hpp"
#include "glm/glm.hpp"            // glm::vec*, mat*, and basic glm functions
#include "glm/gtx/transform.hpp"  // glm::translate, scale, rotate
#include "glm/gtc/type_ptr.hpp" // glm::value_ptr

UniformVariable *GLWidget::s_skybox = NULL;
UniformVariable *GLWidget::s_projection = NULL;
UniformVariable *GLWidget::s_model = NULL;
UniformVariable *GLWidget::s_view = NULL;
UniformVariable *GLWidget::s_mvp = NULL;
UniformVariable *GLWidget::s_time = NULL;
UniformVariable *GLWidget::s_size = NULL;
UniformVariable *GLWidget::s_mouse = NULL;
UniformVariable *GLWidget::s_normalMap = NULL;
UniformVariable *GLWidget::s_textureMap = NULL;

std::vector<UniformVariable*> *GLWidget::s_staticVars = NULL;

QGLShaderProgram *selected_shader = nullptr;

GLWidget::GLWidget(QGLFormat format, QWidget *parent)
    : QGLWidget(format, parent), m_sphere(nullptr), m_cube(nullptr), m_shape(nullptr), skybox_cube(nullptr),
      m_tree(std::make_unique<Tree>()),
      m_textureID(0)
{
    camera = new OrbitingCamera();
    QObject::connect(camera, SIGNAL(viewChanged(glm::mat4)), this, SLOT(viewChanged(glm::mat4)));
    QObject::connect(camera, SIGNAL(projectionChanged(glm::mat4)), this, SLOT(projectionChanged(glm::mat4)));
    QObject::connect(camera, SIGNAL(modelviewProjectionChanged(glm::mat4)), this, SLOT(modelviewProjectionChanged(glm::mat4)));

    activeUniforms = new QList<const UniformVariable *>();
    current_shader = NULL;
    wireframe_shader2 = NULL;

    timer = new QTimer(this);
    connect(timer, SIGNAL(timeout()), this, SLOT(update()));
    timer->start(1000.0f/60.0f);

    s_staticVars = new std::vector<UniformVariable*>();

    changeAnimMode(ANIM_NONE);

    drawWireframe = false;
    wireframeMode = WIREFRAME_NORMAL;
    mouseDown = false;
    setMouseTracking(true);

    settings.loadSettingsOrDefaults();
}

GLWidget::~GLWidget() {
    delete camera;

    delete activeUniforms;
    delete timer;

    delete skybox_shader;
    delete wireframe_shader;

    foreach (const UniformVariable *v, permUniforms) {
        delete v;
    }

    glDeleteTextures(1, &m_textureID);
}

bool GLWidget::saveUniforms(QString path)
{
    QList<const UniformVariable *> toSave;
    foreach (const UniformVariable *v, *activeUniforms) {
        toSave += v;
    }
    foreach (const UniformVariable *v, permUniforms) {
        toSave += v;
    }

    return VarsFile::save(path, &toSave);
}

bool GLWidget::loadUniforms(QString path)
{
    QList<const UniformVariable*> fromFile;

    foreach (const UniformVariable *v, permUniforms) {
        delete v;
    }
    permUniforms.clear();

    if (!VarsFile::load(path, &fromFile, context()->contextHandle())) return false;

    bool match;
    foreach (const UniformVariable *v, fromFile) {
        match = false;
        foreach (const UniformVariable *u, *activeUniforms) {
            if (!v->getName().compare(u->getName()) && v->getType() == u->getType()) {
                // Really really really bad, but the best option I can think of for now
                // sskybox_shaderetPermanent doesn't really modify the object much, sort of a small
                // flag for how to save it
                UniformVariable *utemp = const_cast<UniformVariable *>(u);
                utemp->setPermanent(v->getPermanent());
                emit(changeUniform(u, v->toString()));
                match = true;
            }
        }

        if (!match && v->getPermanent()) {
            permUniforms += v;
        } else {
            delete v;
        }
    }
    return true;
}

void GLWidget::initializeGL() {
    ResourceLoader::initializeGlew();

    glClearColor(0.5, 0.5, 0.5, 1.0);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_TEXTURE_CUBE_MAP);

    glDisable(GL_COLOR_MATERIAL);
    glCullFace(GL_BACK);
    glEnable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

    skybox_shader = ResourceLoader::newShaderProgram(context(), ":/shaders/skybox.vert", ":/shaders/skybox.frag");
    wireframe_shader = ResourceLoader::newShaderProgram(context(), ":/shaders/standard.vert", ":/shaders/color.frag");
    phong_shader = ResourceLoader::newShaderProgram(context(), ":/shaders/light.vert", ":/shaders/light.frag");
    leaf_shader = ResourceLoader::newShaderProgram(context(), ":/shaders/leaf.vert", ":/shaders/leaf.frag");
    normal_mapping_shader = ResourceLoader::newShaderProgram(context(), ":/shaders/normal_map.vert", ":/shaders/normal_map.frag");
    island_shader = ResourceLoader::newShaderProgram(context(), ":/shaders/island.vert", ":/shaders/island.frag");
    glass_shader = ResourceLoader::newShaderProgram(context(), ":/shaders/glass.vert", ":/shaders/glass.frag");

    s_skybox = new UniformVariable(this->context()->contextHandle());
    s_skybox->setName("skybox");
    s_skybox->setType(UniformVariable::TYPE_TEXCUBE);
    //top, bottom, left, right, front, back
    s_skybox->parse(":/skybox/posy.jpg,:/skybox/negy.jpg,:/skybox/negx.jpg,:/skybox/posx.jpg,:/skybox/posz.jpg,:/skybox/negz.jpg");

    s_model = new UniformVariable(this->context()->contextHandle());
    s_model->setName("model");
    s_model->setType(UniformVariable::TYPE_MAT4);
    s_model->parse("1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1");

    s_projection = new UniformVariable(this->context()->contextHandle());
    s_projection->setName("projection");
    s_projection->setType(UniformVariable::TYPE_MAT4);

    s_view = new UniformVariable(this->context()->contextHandle());
    s_view->setName("view");
    s_view->setType(UniformVariable::TYPE_MAT4);

    s_mvp = new UniformVariable(this->context()->contextHandle());
    s_mvp->setName("mvp");
    s_mvp->setType(UniformVariable::TYPE_MAT4);

    s_time = new UniformVariable(this->context()->contextHandle());
    s_time->setName("time");
    s_time->setType(UniformVariable::TYPE_TIME);

    s_size = new UniformVariable(this->context()->contextHandle());
    s_size->setName("size");
    s_size->setType(UniformVariable::TYPE_FLOAT2);

    s_mouse = new UniformVariable(this->context()->contextHandle());
    s_mouse->setName("mouse");
    s_mouse->setType(UniformVariable::TYPE_FLOAT3);

    s_normalMap = new UniformVariable(this->context()->contextHandle());
    s_normalMap->setName("normalMap");
    s_normalMap->setType(UniformVariable::TYPE_TEX2D);
    s_normalMap->parse(":/images/images/brickwall_normal.jpg");

    s_staticVars->push_back(s_skybox);
    s_staticVars->push_back(s_model);
    s_staticVars->push_back(s_projection);
    s_staticVars->push_back(s_view);
    s_staticVars->push_back(s_mvp);
    s_staticVars->push_back(s_time);
    s_staticVars->push_back(s_size);
    s_staticVars->push_back(s_mouse);

    s_staticVars->push_back(s_normalMap);

    gl = QOpenGLFunctions(context()->contextHandle());

    const int NUM_FLOATS_PER_VERTEX = 11; // 3(vert) + 3(norm) + 2(uv) + 3(tangent)

    std::unique_ptr<Shape> sphere = std::make_unique<Cone>(1, 20);
    std::vector<GLfloat> sphereData = sphere->getData();
    m_sphere = std::make_unique<OpenGLShape>();
    m_sphere->setVertexData(&sphereData[0], sphereData.size(), VBO::GEOMETRY_LAYOUT::LAYOUT_TRIANGLES, sphereData.size() / NUM_FLOATS_PER_VERTEX);
    m_sphere->setAttribute(ShaderAttrib::POSITION, 3, 0, VBOAttribMarker::DATA_TYPE::FLOAT, false);
    m_sphere->setAttribute(ShaderAttrib::NORMAL, 3, 3*sizeof(GLfloat), VBOAttribMarker::DATA_TYPE::FLOAT, false);
    m_sphere->setAttribute(ShaderAttrib::TEXCOORD, 2, (3+3)*sizeof(GLfloat), VBOAttribMarker::DATA_TYPE::FLOAT, false);
    m_sphere->setAttribute(ShaderAttrib::TANGENT, 3, (2+3+3)*sizeof(GLfloat), VBOAttribMarker::DATA_TYPE::FLOAT, false);
    m_sphere->buildVAO();

    std::unique_ptr<Shape> test = std::make_unique<Leaf>(6, 1);
    std::vector<GLfloat> testData = test->getData();
    m_cube = std::make_unique<OpenGLShape>();
    m_cube->setVertexData(&testData[0], testData.size(), VBO::GEOMETRY_LAYOUT::LAYOUT_TRIANGLES, testData.size() / NUM_FLOATS_PER_VERTEX);
    m_cube->setAttribute(ShaderAttrib::POSITION, 3, 0, VBOAttribMarker::DATA_TYPE::FLOAT, false);
    m_cube->setAttribute(ShaderAttrib::NORMAL, 3, 3*sizeof(GLfloat), VBOAttribMarker::DATA_TYPE::FLOAT, false);
    m_cube->setAttribute(ShaderAttrib::TEXCOORD, 2, (3+3)*sizeof(GLfloat), VBOAttribMarker::DATA_TYPE::FLOAT, false);
    m_cube->setAttribute(ShaderAttrib::TANGENT, 3, (2+3+3)*sizeof(GLfloat), VBOAttribMarker::DATA_TYPE::FLOAT, false);
    m_cube->buildVAO();

    std::vector<GLfloat> cubeData = CUBE_DATA_POSITIONS;

    skybox_cube = std::make_unique<OpenGLShape>();
    skybox_cube->setVertexData(&cubeData[0], cubeData.size(), VBO::GEOMETRY_LAYOUT::LAYOUT_TRIANGLES, NUM_CUBE_VERTICES);
    skybox_cube->setAttribute(ShaderAttrib::POSITION, 3, 0, VBOAttribMarker::DATA_TYPE::FLOAT, false);
    skybox_cube->setAttribute(ShaderAttrib::NORMAL, 3, 3*sizeof(GLfloat), VBOAttribMarker::DATA_TYPE::FLOAT, false);
    skybox_cube->buildVAO();

    m_cylinder = std::make_unique<OpenGLShape>();
    std::unique_ptr<Shape> cyl = std::make_unique<Cylinder>(1, 7);
    std::vector<GLfloat> cylinderData = cyl->getData();
    m_cylinder = std::make_unique<OpenGLShape>();
    m_cylinder->setVertexData(&cylinderData[0], cylinderData.size(), VBO::GEOMETRY_LAYOUT::LAYOUT_TRIANGLES, cylinderData.size() / NUM_FLOATS_PER_VERTEX);
    m_cylinder->setAttribute(ShaderAttrib::POSITION, 3, 0, VBOAttribMarker::DATA_TYPE::FLOAT, false);
    m_cylinder->setAttribute(ShaderAttrib::NORMAL, 3, 3*sizeof(GLfloat), VBOAttribMarker::DATA_TYPE::FLOAT, false);
    m_cylinder->setAttribute(ShaderAttrib::TEXCOORD, 2, (3+3)*sizeof(GLfloat), VBOAttribMarker::DATA_TYPE::FLOAT, false);
    m_cylinder->setAttribute(ShaderAttrib::TANGENT, 3, (2+3+3)*sizeof(GLfloat), VBOAttribMarker::DATA_TYPE::FLOAT, false);
    m_cylinder->buildVAO();

    m_cone = std::make_unique<OpenGLShape>();
    std::unique_ptr<Shape> cone = std::make_unique<Cone>(1, 7);
    std::vector<GLfloat> coneData = cone->getData();
    m_cone = std::make_unique<OpenGLShape>();
    m_cone->setVertexData(&coneData[0], coneData.size(), VBO::GEOMETRY_LAYOUT::LAYOUT_TRIANGLES, coneData.size() / NUM_FLOATS_PER_VERTEX);
    m_cone->setAttribute(ShaderAttrib::POSITION, 3, 0, VBOAttribMarker::DATA_TYPE::FLOAT, false);
    m_cone->setAttribute(ShaderAttrib::NORMAL, 3, 3*sizeof(GLfloat), VBOAttribMarker::DATA_TYPE::FLOAT, false);
    m_cone->setAttribute(ShaderAttrib::TEXCOORD, 2, (3+3)*sizeof(GLfloat), VBOAttribMarker::DATA_TYPE::FLOAT, false);
    m_cone->setAttribute(ShaderAttrib::TANGENT, 3, (2+3+3)*sizeof(GLfloat), VBOAttribMarker::DATA_TYPE::FLOAT, false);
    m_cone->buildVAO();

    m_island = std::make_unique<OpenGLShape>();
    std::unique_ptr<ShapeComponent> island = std::make_unique<Island>(4, 10, glm::mat4());
    std::vector<GLfloat> islandData = island->getData();
    m_island = std::make_unique<OpenGLShape>();
    m_island->setVertexData(&islandData[0], islandData.size(), VBO::GEOMETRY_LAYOUT::LAYOUT_TRIANGLES, islandData.size()); // NUM_FLOATS_PER_VERTEX);
    m_island->setAttribute(ShaderAttrib::POSITION, 3, 0, VBOAttribMarker::DATA_TYPE::FLOAT, false);
    m_island->setAttribute(ShaderAttrib::NORMAL, 3, 3*sizeof(GLfloat), VBOAttribMarker::DATA_TYPE::FLOAT, false);
    m_island->setAttribute(ShaderAttrib::TEXCOORD, 2, (3+3)*sizeof(GLfloat), VBOAttribMarker::DATA_TYPE::FLOAT, false);
    m_island->setAttribute(ShaderAttrib::TANGENT, 3, (2+3+3)*sizeof(GLfloat), VBOAttribMarker::DATA_TYPE::FLOAT, false);
    m_island->buildVAO();

    m_shape = m_sphere.get();

    glGenTextures(1, &m_textureID);
    glBindTexture(GL_TEXTURE_2D, m_textureID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    QImage image(":/images/images/bark_normal.jpg");
    if (!image.isNull()) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.width(), image.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, image.bits());
    } else {
        std::cout << "Failed to load texture image" << std::endl;
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    selected_shader = phong_shader;
}

void GLWidget::resizeGL(int w, int h) {
    glViewport(0, 0, w, h);
    s_size->parse(QString("%1,%2").arg(QString::number(w), QString::number(h)));
    camera->setAspectRatio(((float) w) / ((float) h));
    update();
}

void GLWidget::handleAnimation() {
    model = glm::mat4();
    switch (animMode) {
    case ANIM_NONE:
        break;
    case ANIM_MOVE_AND_SCALE:
    case ANIM_SCALE:
        if (scale > 2) dscale *= -1;
        if (scale < .5) dscale *= -1;
        scale += scale * dscale;
        model = glm::scale(model, glm::vec3(scale));
        if (animMode == ANIM_SCALE)
            break;
    case ANIM_MOVE:
        if (pos.y > 2) {
            dir *= -1;
        }
        if (pos.y < -2) {
            dir *= -1;
        }
        pos += dir;
        model = glm::translate(model, pos);
        break;
    case ANIM_ROTATE:
        angle += dangle;
        model = glm::rotate(model, degreesToRadians(angle), glm::vec3(0,1,0));
        break;
    case ANIM_ROTATE_2:
        angle += dangle;
        model = glm::rotate(model, degreesToRadians(angle), glm::vec3(0, 0, 1));
        model = glm::translate(model, glm::vec3(0, 2, 0));
        break;
    default:
        break;
    }
    modelChanged(model);
    modelviewProjectionChanged(camera->getProjectionMatrix() * camera->getModelviewMatrix());
}

// Refactored out of the paintGL class for flexibility
void GLWidget::bindAndUpdateShader(QGLShaderProgram *shader) {
    if (shader) {
        shader->bind();
        foreach (const UniformVariable *var, *activeUniforms) {
            var->setValue(shader);
        }
    }
}

void GLWidget::releaseShader(QGLShaderProgram *shader) {
    if (shader) {
        shader->release();
    }
}

// Broken for trees, probably doens't matter.
void GLWidget::renderWireframe() {
    if (drawWireframe) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        switch(wireframeMode) {
        case WIREFRAME_NORMAL:
            wireframe_shader->bind();
            s_mvp->setValue(wireframe_shader);
            wireframe_shader->setUniformValue("color", 0, 0, 0, 1);
            m_shape->draw();
            wireframe_shader->release();
            break;
        case WIREFRAME_VERT:
            wireframe_shader2->bind();
            foreach (const UniformVariable *var, *activeUniforms) {
                var->setValue(wireframe_shader2);
            }
            wireframe_shader2->setUniformValue("color", 0, 0, 0, 1);
            m_shape->draw();
            wireframe_shader2->release();
            break;
        }

        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
}
void GLWidget::renderBranches() {
    //  Note: the wireframes won't work because it's not connected to that,
    // must choose a shader to get it working.

    std::vector<glm::mat4> body = m_tree->getBranchData().body;
    glm::mat4 original = model;
    RenderType oldRenderType = m_renderMode;

    changeRenderMode(SHAPE_CYLINDER);
    for (int i = 0; i < static_cast<int>(body.size()); i++) {
        model = body[i];
        modelChanged(model);
        modelviewProjectionChanged(camera->getProjectionMatrix() * camera->getModelviewMatrix());
        // TODO: restore as current_shader
        bindAndUpdateShader(selected_shader); // needed before calling draw.

        glBindTexture(GL_TEXTURE_2D, m_textureID);
        m_shape->draw();
        glBindTexture(GL_TEXTURE_2D, 0);

    }

    changeRenderMode(SHAPE_CONE);
    std::vector<glm::mat4> tips = m_tree->getBranchData().tip;

    for (int i = 0; i < static_cast<int>(tips.size()); i++) {
        model = tips[i];
        modelChanged(model);
        modelviewProjectionChanged(camera->getProjectionMatrix() * camera->getModelviewMatrix());

        // TODO: restore as current_shader
        bindAndUpdateShader(selected_shader); // needed before calling draw.
        glBindTexture(GL_TEXTURE_2D, m_textureID);
        m_shape->draw();
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    changeRenderMode(oldRenderType);
    model = original; // resets model back to the init
    // TODO: restore as current_shader
    releaseShader(selected_shader);
}

void GLWidget::renderLeaves() {
    std::vector<glm::mat4> trans = m_tree->getLeafData();

    glm::mat4 oldModel = model;
    glm::mat4 original = model;
    RenderType oldRenderType = m_renderMode;

    changeRenderMode(SHAPE_LEAF);
    for (int i = 0; i < static_cast<int>(trans.size()); i++) {
        model = trans[i];
        modelChanged(model);
        modelviewProjectionChanged(camera->getProjectionMatrix() * camera->getModelviewMatrix());

        //Set color based on season
        if (settings.season == 0){
            leaf_shader->setUniformValue("color", QVector4D(0.13f, 0.54f, 0.12f, 0.f));
        } else if (settings.season == 1){
            leaf_shader->setUniformValue("color", QVector4D(0.9f, 0.6f, 0.3f, 0.f));
        } else {
            leaf_shader->setUniformValue("color", QVector4D(0.2f, 0.8f, 0.3f, 0.f));
        }

        bindAndUpdateShader(leaf_shader); // needed before calling draw.
        m_shape->draw();
    }
    // reset states
    changeRenderMode(oldRenderType); // honestly not sure if this should happen in for loop :0
    model = original;
    releaseShader(leaf_shader);
}


void GLWidget::renderSingleLeaf() {
    glm::mat4 oldModel = model;
    model = glm::scale(glm::mat4(), glm::vec3(settings.leafSize, .5, 1.f));
    modelChanged(model);
    modelviewProjectionChanged(camera->getProjectionMatrix() * camera->getModelviewMatrix());
    bindAndUpdateShader(leaf_shader);

    //Set color based on season
    if (settings.season == 0){
        leaf_shader->setUniformValue("color", QVector4D(0.2f, .8f, 0.3f, 0.f));
    } else if (settings.season == 1){
        leaf_shader->setUniformValue("color", QVector4D(0.9f, 0.6f, 0.3f, 0.f));
    } else {
        leaf_shader->setUniformValue("color", QVector4D(0.2f, 0.8f, 0.3f, 0.f));
    }

    m_shape->draw();
    releaseShader(leaf_shader);
}

void GLWidget::renderIsland() {
    RenderType oldRenderType = m_renderMode;
    glm::mat4 scale = glm::scale(glm::mat4(), glm::vec3(1.f, .2f, 1.f));
    glm::mat4 translate = glm::translate(glm::mat4(), glm::vec3(0.f, -.55f, 0.f));

    model = translate * scale * model;
    modelChanged(model);
    modelviewProjectionChanged(camera->getProjectionMatrix() * camera->getModelviewMatrix());

    bindAndUpdateShader(glass_shader);

    changeRenderMode(SHAPE_ISLAND);
    m_shape->draw();

    releaseShader(glass_shader);
    changeRenderMode(oldRenderType);
}

// TODO: any changes to the UI component should also add to this function.
bool GLWidget::hasSettingsChanged() {
    if (m_settings.treeOption != settings.treeOption){
        m_settings.treeOption = settings.treeOption;
        return true;
    }

    if (m_settings.season != settings.season){
        m_settings.season = settings.season;
        return true;
    }
    if (m_settings.recursions != settings.recursions ||
            m_settings.angle != settings.angle) {
        m_settings.recursions = settings.recursions;
        m_settings.angle = settings.angle;
        return true;
    } if (m_settings.leafSize != settings.leafSize) {
        m_settings.leafSize = settings.leafSize;
        return true;
    } if (m_settings.ifBumpMap != settings.ifBumpMap) {
        m_settings.ifBumpMap = settings.ifBumpMap;
        return true;
    }

    return false;
}

void GLWidget::renderSkybox() {
    skybox_shader->bind();
    s_skybox->setValue(skybox_shader);
    s_projection->setValue(skybox_shader);
    s_view->setValue(skybox_shader);
    glCullFace(GL_FRONT);
    skybox_cube->draw();
    glCullFace(GL_BACK);
    skybox_shader->release();
}

void GLWidget::paintGL() {
    handleAnimation();
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);


    selected_shader = settings.ifBumpMap ? normal_mapping_shader : phong_shader;

    if (m_shape) {
        if (m_renderMode == SHAPE_TREE) {
            if (hasSettingsChanged()) {
                m_tree->buildTree(model, settings.leafSize);
            } else {
                renderBranches();
                renderLeaves();
                renderIsland();
            }
        } else {// todo: remove this once texture mapping is done, along with the corresponding button.

            bindAndUpdateShader(selected_shader);
            glBindTexture(GL_TEXTURE_2D, m_textureID);
            m_shape->draw();
            glBindTexture(GL_TEXTURE_2D, 0);
            releaseShader(selected_shader);
        }
        renderWireframe();
    }
    renderSkybox();
}

// Determines the render mode to determine which primitive to draw.
void GLWidget::changeRenderMode(RenderType mode)
{
    m_renderMode = mode;
    switch(m_renderMode) {
    case SHAPE_SPHERE:
        m_shape = m_sphere.get();
        break;
    case SHAPE_CUBE:
        m_shape = m_cube.get();
        break;
    case SHAPE_CYLINDER:
        m_shape = m_cylinder.get();
        break;
    case SHAPE_CONE:
        m_shape = m_cone.get();
        break;
    case SHAPE_ISLAND:
        m_shape = m_island.get();
        break;
    case SHAPE_LEAF:
        m_shape = m_cube.get();
        break;
    default:
        m_shape = m_cylinder.get();
        break;
    }
}

void GLWidget::changeAnimMode(AnimType mode)
{
    model = glm::mat4();
    animMode = mode;
    pos = glm::vec3(0);
    dir = glm::vec3(0,.03,0);
    scale = 1;
    dscale = .017;
    angle = 0;
    dangle = 2;
}

void GLWidget::toggleDrawWireframe(bool draw)
{
    drawWireframe = draw;
}

void GLWidget::setWireframeMode(WireframeType mode)
{
    wireframeMode = mode;
}

bool GLWidget::loadShader(QString vert, QString frag, QString *errors)
{
    QGLShaderProgram *new_shader = ResourceLoader::newShaderProgram(context(), vert, frag, errors);
    if (new_shader == NULL) {
        return false;
    }

    delete wireframe_shader2;
    wireframe_shader2 = ResourceLoader::newShaderProgram(context(), vert, ":/shaders/color.frag", errors);

    UniformVariable::s_numTextures = 2;

    UniformVariable::resetTimer();

    // http://stackoverflow.com/questions/440144/in-opengl-is-there-a-way-to-get-a-list-of-all-uniforms-attribs-used-by-a-shade

    std::vector<GLchar> nameData(256);
    GLint numActiveUniforms = 0;
    gl.glGetProgramiv(new_shader->programId(), GL_ACTIVE_UNIFORMS, &numActiveUniforms);

    for (int unif = 0; unif < numActiveUniforms; unif++) {
        GLint arraySize = 0;
        GLenum type = 0;
        GLsizei actualLength = 0;
        gl.glGetActiveUniform(new_shader->programId(), unif, nameData.size(), &actualLength, &arraySize, &type, &nameData[0]);
        std::string name((char*)&nameData[0], actualLength);

        UniformVariable::Type uniformType = UniformVariable::typeFromGLEnum(type);

        QString qname = QString::fromStdString(name);
        if (qname.startsWith("gl_")) continue;
        emit(addUniform(uniformType, qname, true, arraySize));
    }

    delete current_shader;
    current_shader = new_shader;
    camera->mouseScrolled(0);
    camera->updateMats();
    update();
    return true;
}

void GLWidget::uniformDeleted(const UniformVariable *uniform)
{
    foreach (UniformVariable *sv, *s_staticVars) {
        if (uniform == sv) return;
    }

    foreach (const UniformVariable *v, *activeUniforms) {
        if (uniform == v)
            delete v;
    }
    activeUniforms->removeAll(uniform);
}

void GLWidget::uniformAdded(const UniformVariable *uniform)
{
    activeUniforms->append(uniform);
}

void GLWidget::viewChanged(const glm::mat4 &modelview)
{
    std::stringstream s;
    const float *data = glm::value_ptr(glm::transpose(modelview));
    for (int i = 0; i < 16; i++) {
        s << data[i];
        if (i < 15)
            s << ",";
    }
    s_view->parse(QString::fromStdString(s.str()));
}

void GLWidget::projectionChanged(const glm::mat4 &projection)
{
    std::stringstream s;
    const float *data = glm::value_ptr(glm::transpose(projection));
    for (int i = 0; i < 16; i++) {
        s << data[i];
        if (i < 15)
            s << ",";
    }
    s_projection->parse(QString::fromStdString(s.str()));
}

void GLWidget::modelviewProjectionChanged(const glm::mat4 &modelviewProjection)
{
    std::stringstream s;
    const float *data = glm::value_ptr(glm::transpose(modelviewProjection * model));
    for (int i = 0; i < 16; i++) {
        s << data[i];
        if (i < 15)
            s << ",";
    }
    s_mvp->parse(QString::fromStdString(s.str()));
}

void GLWidget::modelChanged(const glm::mat4 &modelview)
{
    std::stringstream s;
    const float *data = glm::value_ptr(glm::transpose(modelview));
    for (int i = 0; i < 16; i++) {
        s << data[i];
        if (i < 15)
            s << ",";
    }
    s_model->parse(QString::fromStdString(s.str()));
}

void GLWidget::setPaused(bool paused)
{
    if (paused) {
        timer->stop();
    } else {
        timer->start(1000.0f/60.0f);
    }
}

void GLWidget::mouseMoveEvent(QMouseEvent *event) {
    if (event->buttons() & Qt::LeftButton) {
        camera->mouseDragged(event->x(), event->y());
    }
    s_mouse->parse(QString("%1,%2,%3").arg(
                       QString::number(event->x()),
                       QString::number(event->y()),
                       QString::number(mouseDown)));
}

void GLWidget::wheelEvent(QWheelEvent *event)
{
    camera->mouseScrolled(event->delta());
}

void GLWidget::mousePressEvent(QMouseEvent *event) {
    camera->mouseDown(event->x(), event->y());
    mouseDown = true;
    s_mouse->parse(QString("%1,%2,%3").arg(
                       QString::number(event->x()),
                       QString::number(event->y()),
                       QString::number(mouseDown)));
}

void GLWidget::mouseReleaseEvent(QMouseEvent *event) {
    mouseDown = false;
    s_mouse->parse(QString("%1,%2,%3").arg(
                       QString::number(event->x()),
                       QString::number(event->y()),
                       QString::number(mouseDown)));
}
