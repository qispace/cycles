#include "interactive_cycles.h"

#include <windows.h>		/* must include this before GL/gl.h */
#include <wingdi.h>
//#include "../../lib/win64_vc15/epoxy/include/epoxy/gl.h"
#include <epoxy/gl.h>
#include <stdio.h>
#include <mutex>

#pragma comment(lib, "opengl32.lib")


using namespace cycles_wrapper;

std::mutex gOpenGLContextLock;

bool InteractiveCycles::InitializeOpenGL(void *windowHWND, void *hDC, void *hRC, void *contextLock)
{
  mOpenGLContextLockPtr = (std::mutex *)contextLock;
  ptr_windowHWND = windowHWND;
  ptr_hDC = hDC;
  ptr_hRC = hRC;
  mUseOuterContext = true;
  return true;
}

bool InteractiveCycles::InitializeOpenGL(void *windowHWND)
{
  mOpenGLContextLockPtr = &gOpenGLContextLock;
  HWND hWnd = (HWND)windowHWND;
  ptr_windowHWND = hWnd;
  ptr_hDC = GetDC(hWnd);

  PIXELFORMATDESCRIPTOR pfd;
  memset(&pfd, 0, sizeof(pfd));
  pfd.nSize = sizeof(pfd);
  pfd.nVersion = 1;
  pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
  pfd.iPixelType = PFD_TYPE_RGBA;
  pfd.cColorBits = 32;

  int pf = ChoosePixelFormat((HDC)ptr_hDC, &pfd);
  if (pf == 0) {
    Log(LOG_TYPE_ERROR, "ChoosePixelFormat() failed");
    return false;
  }

  if (SetPixelFormat((HDC)ptr_hDC, pf, &pfd) == FALSE) {
    Log(LOG_TYPE_ERROR, "SetPixelFormat() failed");
    return false;
  }

  DescribePixelFormat((HDC)ptr_hDC, pf, sizeof(PIXELFORMATDESCRIPTOR), &pfd);
  ptr_hRC = wglCreateContext((HDC)ptr_hDC);
  mUseOuterContext = false;
  return true;
}

bool InteractiveCycles::DeinitializeOpenGL()
{
  std::lock_guard<std::mutex> lock(*(std::mutex *)mOpenGLContextLockPtr);
  HWND hWnd = (HWND)ptr_windowHWND;
  HDC hDC = (HDC)ptr_hDC;
  HGLRC hRC = (HGLRC)ptr_hRC;

  BOOL result = TRUE;
  if (!mUseOuterContext) {
    result &= wglDeleteContext(hRC);
    result &= (ReleaseDC(hWnd, hDC) == 1);
  }

  mUseOuterContext = false;
  ptr_windowHWND = nullptr;
  ptr_hDC = nullptr;
  ptr_hRC = nullptr;
  return result;
}

//void InteractiveCycles::Draw()
//{
//  EnableContextOpenGL();
//
//  glViewport(0, 0, gw, gh);
//  glClear(GL_COLOR_BUFFER_BIT);
//  glBegin(GL_TRIANGLES);
//  glColor3f(1.0f, 0.0f, 0.0f);
//  glVertex2f(gx + 0.0f, gy + 0.1f);
//  glColor3f(0.0f, 1.0f, 0.0f);
//  glVertex2f(gx - 0.1f, gy - 0.1f);
//  glColor3f(0.0f, 0.0f, 1.0f);
//  glVertex2f(gx + 0.1f, gy - 0.1f);
//  glEnd();
//  glFlush();
//
//  DisableContextOpenGL();
//}

void InteractiveCycles::DrawWithOuterContext_DisplayDriverDelegate(
    int stage, DisplayDriverDelegateParams *params)
{
  switch (stage) {

    case 0: {
      if (params->gl_upload_sync_) {
        glWaitSync((GLsync)params->gl_upload_sync_, 0, GL_TIMEOUT_IGNORED);
      }

      glEnable(GL_BLEND);
      glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    } break;

    case 1: {
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, params->texId);

      if (params->useNearestPointSampling) {
        /* Resolution divider is different from 1, force nearest interpolation. */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      }
      else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      }

      glBindBuffer(GL_ARRAY_BUFFER, params->vertexBufferId);
    } break;

    case 2: {
      GLuint vertex_array_object;
      glGenVertexArrays(1, &vertex_array_object);
      glBindVertexArray(vertex_array_object);
      glEnableVertexAttribArray(params->texcoordAttribute);
      glEnableVertexAttribArray(params->positionAttribute);

      glVertexAttribPointer(
          params->texcoordAttribute, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (const GLvoid *)0);
      glVertexAttribPointer(params->positionAttribute,
                            2,
                            GL_FLOAT,
                            GL_FALSE,
                            4 * sizeof(float),
                            (const GLvoid *)(sizeof(float) * 2));

      glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

      glBindBuffer(GL_ARRAY_BUFFER, 0);
      glBindTexture(GL_TEXTURE_2D, 0);

      glDeleteVertexArrays(1, &vertex_array_object);
      glUseProgram(0);
    } break;

    case 3: {
      glDisable(GL_BLEND);

      params->gl_render_sync_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
      //glFlush();
    } break;

    default:
      break;
  }
}

ACQUIRE_LOCK bool InteractiveCycles::EnableContextOpenGL()
{
  ((std::mutex *)mOpenGLContextLockPtr)->lock();
  HDC hDC = (HDC)ptr_hDC;
  HGLRC hRC = (HGLRC)ptr_hRC;

  bool isOk = wglMakeCurrent(hDC, hRC);
  return isOk;
}

RELEASE_LOCK bool InteractiveCycles::DisableContextOpenGL()
{
  bool isOk = wglMakeCurrent(NULL, NULL);
  ((std::mutex *)mOpenGLContextLockPtr)->unlock();
  return isOk;
}

void InteractiveCycles::SetViewportOpenGL()
{
  glViewport(0, 0, mViewportWidth, mViewportHeight);
}

bool InteractiveCycles::IsOpenGLInitialized()
{
  return (ptr_windowHWND != nullptr && ptr_hDC != nullptr && ptr_hRC != nullptr);
}
