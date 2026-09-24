#pragma once
#include <SDL.h>
#include <SDL_opengl.h>
#include <stdexcept>
#include <string>

namespace seed {
// Explicit dispatch keeps modern OpenGL loading identical on Windows and Linux.
#define SEED_GL_FUNCTIONS(X)                                                                                 \
    X(void, GenVertexArrays, (GLsizei, GLuint*))                                                             \
    X(void, BindVertexArray, (GLuint))                                                                       \
    X(void, DeleteVertexArrays, (GLsizei, const GLuint*))                                                    \
    X(void, GenBuffers, (GLsizei, GLuint*))                                                                  \
    X(void, BindBuffer, (GLenum, GLuint))                                                                    \
    X(void, BufferData, (GLenum, GLsizeiptr, const void*, GLenum))                                           \
    X(void, BufferSubData, (GLenum, GLintptr, GLsizeiptr, const void*))                                      \
    X(void, DeleteBuffers, (GLsizei, const GLuint*))                                                         \
    X(void, EnableVertexAttribArray, (GLuint))                                                               \
    X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*))                   \
    X(void, VertexAttribDivisor, (GLuint, GLuint))                                                           \
    X(void, DrawArraysInstanced, (GLenum, GLint, GLsizei, GLsizei))                                          \
    X(GLuint, CreateShader, (GLenum))                                                                        \
    X(void, ShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*))                             \
    X(void, CompileShader, (GLuint))                                                                         \
    X(void, GetShaderiv, (GLuint, GLenum, GLint*))                                                           \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                          \
    X(void, DeleteShader, (GLuint))                                                                          \
    X(GLuint, CreateProgram, ())                                                                             \
    X(void, AttachShader, (GLuint, GLuint))                                                                  \
    X(void, LinkProgram, (GLuint))                                                                           \
    X(void, GetProgramiv, (GLuint, GLenum, GLint*))                                                          \
    X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                         \
    X(void, DeleteProgram, (GLuint))                                                                         \
    X(void, UseProgram, (GLuint))                                                                            \
    X(GLint, GetUniformLocation, (GLuint, const GLchar*))                                                    \
    X(void, Uniform1i, (GLint, GLint))                                                                       \
    X(void, Uniform1f, (GLint, GLfloat))                                                                     \
    X(void, Uniform2f, (GLint, GLfloat, GLfloat))                                                            \
    X(void, Uniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))                                          \
    X(void, GenTextures, (GLsizei, GLuint*))                                                                 \
    X(void, BindTexture, (GLenum, GLuint))                                                                   \
    X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*))        \
    X(void, CompressedTexImage2D, (GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void*))    \
    X(void, TexParameteri, (GLenum, GLenum, GLint))                                                          \
    X(void, DeleteTextures, (GLsizei, const GLuint*))                                                        \
    X(void, ActiveTexture, (GLenum))                                                                         \
    X(void, Enable, (GLenum))                                                                                \
    X(void, Disable, (GLenum))                                                                               \
    X(void, BlendFunc, (GLenum, GLenum))                                                                     \
    X(void, GenFramebuffers, (GLsizei, GLuint*))                                                             \
    X(void, BindFramebuffer, (GLenum, GLuint))                                                               \
    X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))                                   \
    X(GLenum, CheckFramebufferStatus, (GLenum))                                                              \
    X(void, DeleteFramebuffers, (GLsizei, const GLuint*))                                                    \
    X(void, DrawBuffers, (GLsizei, const GLenum*))                                                           \
    X(void, Viewport, (GLint, GLint, GLsizei, GLsizei))                                                      \
    X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                                \
    X(void, Clear, (GLbitfield))                                                                             \
    X(GLenum, GetError, ())                                                                                  \
    X(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*))

struct Gl {
#define SEED_DECLARE(ret, name, args)                                                                        \
    using name##Fn = ret(APIENTRY*) args;                                                                    \
    name##Fn name{};
    SEED_GL_FUNCTIONS(SEED_DECLARE)
#undef SEED_DECLARE
    Gl() {
#define SEED_LOAD(ret, name, args)                                                                           \
    name = reinterpret_cast<name##Fn>(SDL_GL_GetProcAddress("gl" #name));                                    \
    if (!name) throw std::runtime_error("Missing OpenGL entry point: gl" #name);
        SEED_GL_FUNCTIONS(SEED_LOAD)
#undef SEED_LOAD
    }
    void check() const {
        const auto error = GetError();
        if (error != GL_NO_ERROR) throw std::runtime_error("OpenGL error " + std::to_string(error));
    }
};
#undef SEED_GL_FUNCTIONS
} // namespace seed
