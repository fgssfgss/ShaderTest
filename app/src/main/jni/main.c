#include <jni.h>
#include <errno.h>
#include <math.h>

#include <EGL/egl.h>
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>


#include <android/sensor.h>
#include <android/log.h>
#include <android_native_app_glue.h>

#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, "native-activity", __VA_ARGS__))
#define LOGW(...) ((void)__android_log_print(ANDROID_LOG_WARN, "native-activity", __VA_ARGS__))

PFNGLGENVERTEXARRAYSOESPROC glGenVertexArraysOES;
PFNGLBINDVERTEXARRAYOESPROC glBindVertexArrayOES;
PFNGLDELETEVERTEXARRAYSOESPROC glDeleteVertexArraysOES;
PFNGLISVERTEXARRAYOESPROC glIsVertexArrayOES;

struct engine {
    struct android_app *app;

    EGLDisplay display;
    EGLSurface surface;
    EGLContext context;
    int32_t width;
    int32_t height;
    int prog;
    int32_t touchX;
    int32_t touchY;

    double time;

    GLuint vertexBuffer;
    GLuint indexBuffer;
    GLuint vertexArray;
    GLuint positionSlot;

    GLuint resolution;
    GLuint globalTime;
    GLuint FragCoord;
};

const float Vertices[] = {
        1.0f, -1.0f, 0.0f,
        1.0f, 1.0f, 0.0f,
        -1.0f, 1.0f, 0.0f,
        -1.0f, -1.0f, 0.0f
};

const GLubyte Indices[] = {
        0, 1, 2,
        2, 3, 0
};

char *shader_vtx =
        "precision mediump float;\n"
                "precision mediump int;\n"
                "attribute vec3 position;\n"
                "void main() {\n"
                "    gl_Position.xyz = position;\n"
                "    gl_Position.w = 1.0;\n"
                "}";

char *shader_frg =
        "#extension GL_OES_standard_derivatives : enable\n"
                "precision mediump float;\n"
                "precision mediump int;\n"
                "uniform vec3      iResolution;\n"
                "uniform float     iGlobalTime;\n"
                "uniform vec2      ifFragCoordOffsetUniform;\n"
                "float world(vec3 a) {\n"
                "return min(min(length(a) - 1., dot(a, vec3(0.,1.,0.)) + 1.), length(a-vec3(1.5*cos(iGlobalTime),0.,1.5*sin(iGlobalTime))) - .2);\n"
                "}\n"
                "\n"
                "float trace(vec3 O, vec3 D) {\n"
                "float L = 0.; \n"
                "for (int i = 0; i < 128; ++i) { \n"
                "float d = world(O + D*L); \n"
                "L += d; \n"
                "if (d < .0001*L) break; \n"
                "}\n"
                "return L; \n"
                "}\n"
                "\n"
                "vec3 wnormal(vec3 a) {\n"
                "vec2 e = vec2(.001, 0.);\n"
                "float w = world(a);\n"
                "return normalize(vec3(world(a+e.xyy) - w,\n"
                "                          world(a+e.yxy) - w,\n"
                "                          world(a+e.yyx) - w));\n"
                "}\n"
                "\n"
                "vec3 enlight(vec3 at, vec3 normal, vec3 diffuse, vec3 l_color, vec3 l_pos) {\n"
                "vec3 l_dir = l_pos - at;\n"
                "return diffuse * l_color * max(0.,dot(normal,normalize(l_dir))) / dot(l_dir, l_dir);\n"
                "}\n"
                "\n"
                "float occlusion(vec3 at, vec3 normal) {\n"
                "float b = 0.;\n"
                "for (int i = 1; i <= 4; ++i) {\n"
                "float L = .06 * float(i);\n"
                "float d = world(at + normal * L);\n"
                "b += max(0., L - d);\n"
                "}\n"
                "return min(b, 1.);\n"
                "}\n"
                "\n"
                "void mainImage( out vec4 fragColor, in vec2 fragCoord )\n"
                "{\n"
                "    vec2 uv = gl_FragCoord.xy / iResolution.xy * 2. - 1.;\n"
                "    uv.x *= iResolution.x / iResolution.y;\n"
                "    vec3 O = vec3(0., 0., 3.);\n"
                "    vec3 D = normalize(vec3(uv, -2.));\n"
                "    float path = trace(O, D);\n"
                "    vec3 position = O+D*path;\n"
                "    vec3 normal_pos = wnormal(position);\n"
                "    vec3 color = vec3(.2) * (1. - occlusion(position, normal_pos));\n"
                "    color += enlight(position, normal_pos, vec3(1.), vec3(1.), vec3(1.,1.,2.));\n"
                "    color = mix(color, vec3(0.), smoothstep(0.,20.,path));\n"
                "    fragColor = vec4(color, 0.);\n"
                "}\n"
                "void main()  {\n"
                "    mainImage(gl_FragColor, gl_FragCoord.xy + ifFragCoordOffsetUniform );\n"
                "    gl_FragColor.w = 1.;\n"
                "}\n";


static void printGLString(const char *name, GLenum s) {
    const char *v = (const char *) glGetString(s);
    LOGI("GL %s = %s\n", name, v);
}

int init_display(struct engine *engine) {
    const EGLint attribs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_BLUE_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_RED_SIZE, 8,
            EGL_DEPTH_SIZE, 24,
            EGL_NONE
    };

    EGLint attribList[] =
            {
                    EGL_CONTEXT_CLIENT_VERSION, 2,
                    EGL_NONE
            };

    EGLint w, h, dummy, format;
    EGLint numConfigs;
    EGLConfig config;
    EGLSurface surface;
    EGLContext context;

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);

    eglInitialize(display, 0, 0);
    eglChooseConfig(display, attribs, &config, 1, &numConfigs);
    eglGetConfigAttrib(display, config, EGL_NATIVE_VISUAL_ID, &format);

    ANativeWindow_setBuffersGeometry(engine->app->window, 0, 0, format);

    surface = eglCreateWindowSurface(display, config, engine->app->window, NULL);

    context = eglCreateContext(display, config, NULL, attribList);

    if (eglMakeCurrent(display, surface, surface, context) == EGL_FALSE) {
        LOGW("Unable to eglMakeCurrent");
        return -1;
    }

    // Grab the width and height of the surface
    eglQuerySurface(display, surface, EGL_WIDTH, &w);
    eglQuerySurface(display, surface, EGL_HEIGHT, &h);

    engine->display = display;
    engine->context = context;
    engine->surface = surface;
    engine->width = w;
    engine->height = h;

    // Initialize GL state.
    glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_FASTEST);
    glEnable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glViewport(0, 0, w, h);

    printGLString("Version", GL_VERSION);
    printGLString("Vendor", GL_VENDOR);
    printGLString("Renderer", GL_RENDERER);
    printGLString("Extensions", GL_EXTENSIONS);
    printGLString("GLSL", GL_SHADER_COMPILER);

    return 0;
}

static void init_shader(struct engine *engine) {
    int program = glCreateProgram();
    int shader_vr = glCreateShader(GL_VERTEX_SHADER);
    int shader_fr = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(shader_vr, 1, &shader_vtx, NULL);
    glCompileShader(shader_vr);
    glAttachShader(program, shader_vr);
    glShaderSource(shader_fr, 1, &shader_frg, NULL);
    glCompileShader(shader_fr);
    glAttachShader(program, shader_fr);
    glLinkProgram(program);

    engine->positionSlot = glGetAttribLocation(program, "position");
    engine->globalTime = glGetUniformLocation(program, "iGlobalTime");
    engine->resolution = glGetUniformLocation(program, "iResolution");
    engine->FragCoord = glGetUniformLocation(program, "ifFragCoordOffsetUniform");

    GLint logLength;
    glGetShaderiv(shader_fr, GL_INFO_LOG_LENGTH, &logLength);
    if (logLength > 0) {
        GLchar *log = (GLchar *) malloc(logLength);
        glGetShaderInfoLog(shader_fr, logLength, &logLength, log);
        LOGI("%s bla bla", log);
        free(log);
    }

    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
    if (logLength > 0) {
        GLchar *log = (GLchar *) malloc(logLength);
        glGetProgramInfoLog(program, logLength, &logLength, log);
        LOGI("%s bla la", log);
        free(log);
    }

    engine->prog = program;
    return;
}

void create_buffers(struct engine *engine) {
    glGenVertexArraysOES(1, &engine->vertexArray);
    glBindVertexArrayOES(engine->vertexArray);

    glGenBuffers(1, &engine->vertexBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, engine->vertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(Vertices), Vertices, GL_STATIC_DRAW);

    glGenBuffers(1, &engine->indexBuffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, engine->indexBuffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(Indices), Indices, GL_STATIC_DRAW);

    glEnableVertexAttribArray(engine->positionSlot);
    glVertexAttribPointer(engine->positionSlot, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float),
                          (const GLvoid *) 0);

    glBindVertexArrayOES(0);
}

static void bind_uniforms(struct engine *engine) {
    glUniform1f(engine->globalTime, engine->time);
    //LOGI("global time %lf", engine->time);
    glUniform3f(engine->resolution, (float) engine->width, (float) engine->height, (float) 1.0f);
    glUniform2f(engine->FragCoord, (float) 1.0f, (float) 1.0f);
}

static double gettime() {
    struct timeval tv;
    gettimeofday(&tv, NULL);

    return (double) (tv.tv_sec);
}

void draw_frame(struct engine *engine) {
    //double t = gettime();
    if (engine->display == NULL) {
        return;
    }

    glViewport(0, 0, engine->width, engine->height);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(engine->prog);
    bind_uniforms(engine);
    glBindVertexArrayOES(engine->vertexArray);
    glDrawElements(GL_TRIANGLES, sizeof(Indices) / sizeof(Indices[0]), GL_UNSIGNED_BYTE, 0);
    eglSwapBuffers(engine->display, engine->surface);
    //LOGI("%lf time for frame", gettime() - t);
}

void terminate_display(struct engine *engine) {
    if (engine->display != EGL_NO_DISPLAY) {
        eglMakeCurrent(engine->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (engine->context != EGL_NO_CONTEXT) {
            eglDestroyContext(engine->display, engine->context);
        }
        if (engine->surface != EGL_NO_SURFACE) {
            eglDestroySurface(engine->display, engine->surface);
        }
        eglTerminate(engine->display);
    }
    engine->display = EGL_NO_DISPLAY;
    engine->context = EGL_NO_CONTEXT;
    engine->surface = EGL_NO_SURFACE;
}

int32_t handle_input(struct android_app *app, AInputEvent *event) {
    struct engine *engine = (struct engine *) app->userData;
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) {
        engine->touchX = AMotionEvent_getX(event, 0);
        engine->touchY = AMotionEvent_getY(event, 0);
        LOGI("x=%d\ty=%d", engine->touchX, engine->touchY);
        return 1;
    }
    return 0;
}

void handle_cmd(struct android_app *app, int32_t cmd) {
    struct engine *engine = (struct engine *) app->userData;
    switch (cmd) {
        case APP_CMD_SAVE_STATE:
            break;
        case APP_CMD_INIT_WINDOW:
            if (engine->app->window != NULL) {
                init_display(engine);
                create_buffers(engine);
                init_shader(engine);
                draw_frame(engine);
            }
            break;
        case APP_CMD_TERM_WINDOW:
            terminate_display(engine);
            break;
        case APP_CMD_LOST_FOCUS:
            draw_frame(engine);
            break;
    }
}

void android_main(struct android_app *state) {
    app_dummy();

    double start_time = 0.0;
    struct engine engine;

    memset(&engine, 0, sizeof(engine));
    state->userData = &engine;
    state->onAppCmd = handle_cmd;
    state->onInputEvent = handle_input;
    engine.app = state;
    engine.time = 0;
    start_time = gettime();

    glGenVertexArraysOES = (PFNGLGENVERTEXARRAYSOESPROC) eglGetProcAddress("glGenVertexArraysOES");
    glBindVertexArrayOES = (PFNGLBINDVERTEXARRAYOESPROC) eglGetProcAddress("glBindVertexArrayOES");
    glDeleteVertexArraysOES = (PFNGLDELETEVERTEXARRAYSOESPROC) eglGetProcAddress(
            "glDeleteVertexArraysOES");
    glIsVertexArrayOES = (PFNGLISVERTEXARRAYOESPROC) eglGetProcAddress("glIsVertexArrayOES");

    while (1) {
        int ident;
        int events;
        struct android_poll_source *source;

        while ((ident = ALooper_pollAll(0, NULL, &events, (void **) &source)) >= 0) {

            if (source != NULL) {
                source->process(state, source);
            }

            if (state->destroyRequested != 0) {
                terminate_display(&engine);
                return;
            }
        }

        draw_frame(&engine);
        engine.time = gettime() - start_time;
    }
}