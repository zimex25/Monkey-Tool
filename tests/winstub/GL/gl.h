// stub of <GL/gl.h> - OpenGL 1.1, only what the 0.9.4 viewer uses
#pragma once
#include <windows.h>
typedef unsigned int GLenum; typedef unsigned int GLuint; typedef int GLint; typedef int GLsizei;
typedef float GLfloat; typedef double GLdouble; typedef unsigned char GLboolean; typedef unsigned int GLbitfield;
typedef void GLvoid; typedef unsigned char GLubyte;
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_PROJECTION 0x1701
#define GL_MODELVIEW 0x1700
#define GL_TEXTURE 0x1702
#define GL_DEPTH_TEST 0x0B71
#define GL_LIGHTING 0x0B50
#define GL_LIGHT0 0x4000
#define GL_LIGHT1 0x4001
#define GL_POSITION 0x1203
#define GL_DIFFUSE 0x1201
#define GL_AMBIENT 0x1200
#define GL_SPECULAR 0x1202
#define GL_LIGHT_MODEL_TWO_SIDE 0x0B52
#define GL_LIGHT_MODEL_AMBIENT 0x0B53
#define GL_COLOR_MATERIAL 0x0B57
#define GL_AMBIENT_AND_DIFFUSE 0x1602
#define GL_FRONT_AND_BACK 0x0408
#define GL_NORMALIZE 0x0BA1
#define GL_TEXTURE_2D 0x0DE1
#define GL_BLEND 0x0BE2
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_LINES 0x0001
#define GL_QUADS 0x0007
#define GL_TRIANGLES 0x0004
#define GL_UNSIGNED_INT 0x1405
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_LUMINANCE 0x1909
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_LINEAR 0x2601
#define GL_REPEAT 0x2901
#define GL_TEXTURE_ENV 0x2300
#define GL_TEXTURE_ENV_MODE 0x2200
#define GL_MODULATE 0x2100
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_VERTEX_ARRAY 0x8074
#define GL_NORMAL_ARRAY 0x8075
#define GL_TEXTURE_COORD_ARRAY 0x8078
#define GL_COLOR_ARRAY 0x8076
#define GL_COMPILE 0x1300
#define GL_SMOOTH 0x1D01
#define GL_LEQUAL 0x0203
#define GL_CULL_FACE 0x0B44
#define GL_POLYGON_OFFSET_FILL 0x8037
#define GL_LINE_SMOOTH 0x0B20
#define GL_TRUE 1
#define GL_FALSE 0
#define GL_STENCIL_TEST 0x0B90
#define GL_STENCIL_BITS 0x0D57
#define GL_STENCIL_BUFFER_BIT 0x00000400
#define GL_EMISSION 0x1600
#define GL_TRIANGLE_FAN 0x0006
#define GL_GREATER 0x0204
#define GL_REPLACE 0x1E01
#define GL_KEEP 0x1E00
#define GL_ALPHA_TEST 0x0BC0
extern "C" {
void glViewport(GLint, GLint, GLsizei, GLsizei);
void glClearColor(GLfloat, GLfloat, GLfloat, GLfloat);
void glClear(GLbitfield);
void glMatrixMode(GLenum);
void glLoadIdentity(void);
void glLoadMatrixf(const GLfloat*);
void glFrustum(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
void glOrtho(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
void glTranslatef(GLfloat, GLfloat, GLfloat);
void glScalef(GLfloat, GLfloat, GLfloat);
void glEnable(GLenum);
void glDisable(GLenum);
void glBegin(GLenum);
void glEnd(void);
void glColor3f(GLfloat, GLfloat, GLfloat);
void glColor4f(GLfloat, GLfloat, GLfloat, GLfloat);
void glVertex2f(GLfloat, GLfloat);
void glVertex3f(GLfloat, GLfloat, GLfloat);
void glNormal3f(GLfloat, GLfloat, GLfloat);
void glLightfv(GLenum, GLenum, const GLfloat*);
void glLightModeli(GLenum, GLint);
void glLightModelfv(GLenum, const GLfloat*);
void glColorMaterial(GLenum, GLenum);
void glShadeModel(GLenum);
void glDepthMask(GLboolean);
void glDepthFunc(GLenum);
void glBlendFunc(GLenum, GLenum);
void glGenTextures(GLsizei, GLuint*);
void glDeleteTextures(GLsizei, const GLuint*);
void glBindTexture(GLenum, GLuint);
void glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid*);
void glTexParameteri(GLenum, GLenum, GLint);
void glTexEnvi(GLenum, GLenum, GLint);
void glPixelStorei(GLenum, GLint);
void glEnableClientState(GLenum);
void glDisableClientState(GLenum);
void glVertexPointer(GLint, GLenum, GLsizei, const GLvoid*);
void glNormalPointer(GLenum, GLsizei, const GLvoid*);
void glTexCoordPointer(GLint, GLenum, GLsizei, const GLvoid*);
void glColorPointer(GLint, GLenum, GLsizei, const GLvoid*);
void glDrawElements(GLenum, GLsizei, GLenum, const GLvoid*);
GLuint glGenLists(GLsizei);
void glNewList(GLuint, GLenum);
void glEndList(void);
void glCallList(GLuint);
void glDeleteLists(GLuint, GLsizei);
void glListBase(GLuint);
void glCallLists(GLsizei, GLenum, const GLvoid*);
void glRasterPos2i(GLint, GLint);
void glLineWidth(GLfloat);
void glPolygonOffset(GLfloat, GLfloat);
void glFlush(void);
void glStencilFunc(GLenum, GLint, GLuint);
void glStencilOp(GLenum, GLenum, GLenum);
void glClearStencil(GLint);
void glMaterialfv(GLenum, GLenum, const GLfloat*);
void glPushMatrix(void);
void glPopMatrix(void);
void glMultMatrixf(const GLfloat*);
void glGetIntegerv(GLenum, GLint*);
void glAlphaFunc(GLenum, GLfloat);
}
/* texgen, blending (1.0) */
#define GL_ONE 1
#define GL_S 0x2000
#define GL_T 0x2001
#define GL_TEXTURE_GEN_MODE 0x2500
#define GL_SPHERE_MAP 0x2402
#define GL_TEXTURE_GEN_S 0x0C60
#define GL_TEXTURE_GEN_T 0x0C61
GLboolean glIsEnabled(GLenum);
void glTexGeni(GLenum, GLenum, GLint);

