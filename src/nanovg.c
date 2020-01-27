//
// Copyright (c) 2013 Mikko Mononen memon@inside.org
//
// This software is provided 'as-is', without any express or implied
// warranty.  In no event will the authors be held liable for any damages
// arising from the use of this software.
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would be
//    appreciated but is not required.
// 2. Altered source versions must be plainly marked as such, and must not be
//    misrepresented as being the original software.
// 3. This notice may not be removed or altered from any source distribution.
//

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <memory.h>

#include "nanovg.h"
#define FONTSTASH_IMPLEMENTATION
#include "fontstash.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#ifdef _MSC_VER
#pragma warning(disable: 4100)  // unreferenced formal parameter
#pragma warning(disable: 4127)  // conditional expression is constant
#pragma warning(disable: 4204)  // nonstandard extension used : non-constant aggregate initializer
#pragma warning(disable: 4706)  // assignment within conditional expression
#endif

#define NVG_INIT_FONTIMAGE_SIZE  512
#define NVG_MAX_FONTIMAGE_SIZE   2048
#define NVG_MAX_FONTIMAGES       4

#define NVG_INIT_COMMANDS_SIZE 256
#define NVG_INIT_POINTS_SIZE 128
#define NVG_INIT_PATHS_SIZE 16
#define NVG_INIT_VERTS_SIZE 256
#define NVG_MAX_STATES 32

#define NVG_KAPPA90 0.5522847493f	// Length proportional to radius of a cubic bezier handle for 90deg arcs.

#define NVG_COUNTOF(arr) (sizeof(arr) / sizeof(0[arr]))


enum NVGcommands {
	NVG_MOVETO = 0,
	NVG_LINETO = 1,
	NVG_BEZIERTO = 2,
	NVG_CLOSE = 3,
	NVG_WINDING = 4,
};

enum NVGpointFlags
{
	NVG_PT_CORNER = 0x01,
	NVG_PT_LEFT = 0x02,
	NVG_PT_BEVEL = 0x04,
	NVG_PR_INNERBEVEL = 0x08,
};

struct NVGstate {
	NVGcompositeOperationState compositeOperation;
	int shapeAntiAlias;
	NVGpaint fill;
	NVGpaint stroke;
	float strokeWidth;
	float miterLimit;
	int lineJoin;
	int lineCap;
	float alpha;
	float xform[6];
	NVGscissor scissor;
	float fontSize;
	float letterSpacing;
	float lineHeight;
	float fontBlur;
	int textAlign;
	int fontId;
};
typedef struct NVGstate NVGstate;

struct NVGpoint {
	float x,y;
	float dx, dy;
	float len;
	float dmx, dmy;
	unsigned char flags;
};
typedef struct NVGpoint NVGpoint;

struct NVGpathCache {
	NVGpoint* points;
	int npoints;
	int cpoints;
	NVGpath* paths;
	int npaths;
	int cpaths;
	NVGvertex* verts;
	int nverts;
	int cverts;
	float bounds[4];
};
typedef struct NVGpathCache NVGpathCache;

typedef struct NVGpickScene NVGpickScene;
void nvg__pickBeginFrame(NVGcontext* ctx, int width, int height);
void nvg__deletePickScene(NVGpickScene* ps);

struct NVGcontext {
	NVGparams params;
	float* commands;
	int ccommands;
	int ncommands;
	float commandx, commandy;
	NVGstate states[NVG_MAX_STATES];
	int nstates;
	NVGpathCache* cache;
	float tessTol;
	float distTol;
	float fringeWidth;
	float devicePxRatio;
	struct FONScontext* fs;
	int fontImages[NVG_MAX_FONTIMAGES];
	int fontImageIdx;
	int drawCallCount;
	int fillTriCount;
	int strokeTriCount;
	int textTriCount;
	NVGpickScene* pickScene;
};

static float nvg__sqrtf(float a) { return sqrtf(a); }
static float nvg__modf(float a, float b) { return fmodf(a, b); }
static float nvg__sinf(float a) { return sinf(a); }
static float nvg__cosf(float a) { return cosf(a); }
static float nvg__tanf(float a) { return tanf(a); }
static float nvg__atan2f(float a,float b) { return atan2f(a, b); }
static float nvg__acosf(float a) { return acosf(a); }

static int nvg__mini(int a, int b) { return a < b ? a : b; }
static int nvg__maxi(int a, int b) { return a > b ? a : b; }
static int nvg__clampi(int a, int mn, int mx) { return a < mn ? mn : (a > mx ? mx : a); }
static float nvg__minf(float a, float b) { return a < b ? a : b; }
static float nvg__maxf(float a, float b) { return a > b ? a : b; }
static float nvg__absf(float a) { return a >= 0.0f ? a : -a; }
static float nvg__signf(float a) { return a >= 0.0f ? 1.0f : -1.0f; }
static float nvg__clampf(float a, float mn, float mx) { return a < mn ? mn : (a > mx ? mx : a); }
static float nvg__cross(float dx0, float dy0, float dx1, float dy1) { return dx1*dy0 - dx0*dy1; }

static float nvg__normalize(float *x, float* y)
{
	float d = nvg__sqrtf((*x)*(*x) + (*y)*(*y));
	if (d > 1e-6f) {
		float id = 1.0f / d;
		*x *= id;
		*y *= id;
	}
	return d;
}


static void nvg__deletePathCache(NVGpathCache* c)
{
	if (c == NULL) return;
	if (c->points != NULL) free(c->points);
	if (c->paths != NULL) free(c->paths);
	if (c->verts != NULL) free(c->verts);
	free(c);
}

static NVGpathCache* nvg__allocPathCache(void)
{
	NVGpathCache* c = (NVGpathCache*)malloc(sizeof(NVGpathCache));
	if (c == NULL) goto error;
	memset(c, 0, sizeof(NVGpathCache));

	c->points = (NVGpoint*)malloc(sizeof(NVGpoint)*NVG_INIT_POINTS_SIZE);
	if (!c->points) goto error;
	c->npoints = 0;
	c->cpoints = NVG_INIT_POINTS_SIZE;

	c->paths = (NVGpath*)malloc(sizeof(NVGpath)*NVG_INIT_PATHS_SIZE);
	if (!c->paths) goto error;
	c->npaths = 0;
	c->cpaths = NVG_INIT_PATHS_SIZE;

	c->verts = (NVGvertex*)malloc(sizeof(NVGvertex)*NVG_INIT_VERTS_SIZE);
	if (!c->verts) goto error;
	c->nverts = 0;
	c->cverts = NVG_INIT_VERTS_SIZE;

	return c;
error:
	nvg__deletePathCache(c);
	return NULL;
}

static void nvg__setDevicePixelRatio(NVGcontext* ctx, float ratio)
{
	ctx->tessTol = 0.25f / ratio;
	ctx->distTol = 0.01f / ratio;
	ctx->fringeWidth = 1.0f / ratio;
	ctx->devicePxRatio = ratio;
}

static NVGcompositeOperationState nvg__compositeOperationState(int op)
{
	int sfactor, dfactor;

	if (op == NVG_SOURCE_OVER)
	{
		sfactor = NVG_ONE;
		dfactor = NVG_ONE_MINUS_SRC_ALPHA;
	}
	else if (op == NVG_SOURCE_IN)
	{
		sfactor = NVG_DST_ALPHA;
		dfactor = NVG_ZERO;
	}
	else if (op == NVG_SOURCE_OUT)
	{
		sfactor = NVG_ONE_MINUS_DST_ALPHA;
		dfactor = NVG_ZERO;
	}
	else if (op == NVG_ATOP)
	{
		sfactor = NVG_DST_ALPHA;
		dfactor = NVG_ONE_MINUS_SRC_ALPHA;
	}
	else if (op == NVG_DESTINATION_OVER)
	{
		sfactor = NVG_ONE_MINUS_DST_ALPHA;
		dfactor = NVG_ONE;
	}
	else if (op == NVG_DESTINATION_IN)
	{
		sfactor = NVG_ZERO;
		dfactor = NVG_SRC_ALPHA;
	}
	else if (op == NVG_DESTINATION_OUT)
	{
		sfactor = NVG_ZERO;
		dfactor = NVG_ONE_MINUS_SRC_ALPHA;
	}
	else if (op == NVG_DESTINATION_ATOP)
	{
		sfactor = NVG_ONE_MINUS_DST_ALPHA;
		dfactor = NVG_SRC_ALPHA;
	}
	else if (op == NVG_LIGHTER)
	{
		sfactor = NVG_ONE;
		dfactor = NVG_ONE;
	}
	else if (op == NVG_COPY)
	{
		sfactor = NVG_ONE;
		dfactor = NVG_ZERO;
	}
	else if (op == NVG_XOR)
	{
		sfactor = NVG_ONE_MINUS_DST_ALPHA;
		dfactor = NVG_ONE_MINUS_SRC_ALPHA;
	}
	else
	{
		sfactor = NVG_ONE;
		dfactor = NVG_ZERO;
	}

	NVGcompositeOperationState state;
	state.srcRGB = sfactor;
	state.dstRGB = dfactor;
	state.srcAlpha = sfactor;
	state.dstAlpha = dfactor;
	return state;
}

static NVGstate* nvg__getState(NVGcontext* ctx)
{
	return &ctx->states[ctx->nstates-1];
}

NVGcontext* nvgCreateInternal(NVGparams* params)
{
	FONSparams fontParams;
	NVGcontext* ctx = (NVGcontext*)malloc(sizeof(NVGcontext));
	int i;
	if (ctx == NULL) goto error;
	memset(ctx, 0, sizeof(NVGcontext));

	ctx->params = *params;
	for (i = 0; i < NVG_MAX_FONTIMAGES; i++)
		ctx->fontImages[i] = 0;

	ctx->commands = (float*)malloc(sizeof(float)*NVG_INIT_COMMANDS_SIZE);
	if (!ctx->commands) goto error;
	ctx->ncommands = 0;
	ctx->ccommands = NVG_INIT_COMMANDS_SIZE;

	ctx->cache = nvg__allocPathCache();
	if (ctx->cache == NULL) goto error;

	nvgSave(ctx);
	nvgReset(ctx);

	nvg__setDevicePixelRatio(ctx, 1.0f);

	if (ctx->params.renderCreate(ctx->params.userPtr) == 0) goto error;

	// Init font rendering
	memset(&fontParams, 0, sizeof(fontParams));
	fontParams.width = NVG_INIT_FONTIMAGE_SIZE;
	fontParams.height = NVG_INIT_FONTIMAGE_SIZE;
	fontParams.flags = FONS_ZERO_TOPLEFT;
	fontParams.renderCreate = NULL;
	fontParams.renderUpdate = NULL;
	fontParams.renderDraw = NULL;
	fontParams.renderDelete = NULL;
	fontParams.userPtr = NULL;
	ctx->fs = fonsCreateInternal(&fontParams);
	if (ctx->fs == NULL) goto error;

	// Create font texture
	ctx->fontImages[0] = ctx->params.renderCreateTexture(ctx->params.userPtr, NVG_TEXTURE_ALPHA, fontParams.width, fontParams.height, 0, NULL);
	if (ctx->fontImages[0] == 0) goto error;
	ctx->fontImageIdx = 0;

	return ctx;

error:
	nvgDeleteInternal(ctx);
	return 0;
}

NVGparams* nvgInternalParams(NVGcontext* ctx)
{
    return &ctx->params;
}

void nvgDeleteInternal(NVGcontext* ctx)
{
	int i;
	if (ctx == NULL) return;
	if (ctx->commands != NULL) free(ctx->commands);
	if (ctx->cache != NULL) nvg__deletePathCache(ctx->cache);

	if (ctx->fs)
		fonsDeleteInternal(ctx->fs);

	for (i = 0; i < NVG_MAX_FONTIMAGES; i++) {
		if (ctx->fontImages[i] != 0) {
			nvgDeleteImage(ctx, ctx->fontImages[i]);
			ctx->fontImages[i] = 0;
		}
	}

	if (ctx->params.renderDelete != NULL)
		ctx->params.renderDelete(ctx->params.userPtr);
	
	if (ctx->pickScene != NULL)
		nvg__deletePickScene(ctx->pickScene);

	free(ctx);
}

void nvgBeginFrame(NVGcontext* ctx, float windowWidth, float windowHeight, float devicePixelRatio)
{
/*	printf("Tris: draws:%d  fill:%d  stroke:%d  text:%d  TOT:%d\n",
		ctx->drawCallCount, ctx->fillTriCount, ctx->strokeTriCount, ctx->textTriCount,
		ctx->fillTriCount+ctx->strokeTriCount+ctx->textTriCount);*/

	ctx->nstates = 0;
	nvgSave(ctx);
	nvgReset(ctx);

	nvg__setDevicePixelRatio(ctx, devicePixelRatio);

	ctx->params.renderViewport(ctx->params.userPtr, windowWidth, windowHeight, devicePixelRatio);

	ctx->drawCallCount = 0;
	ctx->fillTriCount = 0;
	ctx->strokeTriCount = 0;
	ctx->textTriCount = 0;
	
	nvg__pickBeginFrame(ctx, windowWidth, windowHeight);
}

void nvgCancelFrame(NVGcontext* ctx)
{
	ctx->params.renderCancel(ctx->params.userPtr);
}

void nvgEndFrame(NVGcontext* ctx)
{
	ctx->params.renderFlush(ctx->params.userPtr);
	if (ctx->fontImageIdx != 0) {
		int fontImage = ctx->fontImages[ctx->fontImageIdx];
		int i, j, iw, ih;
		// delete images that smaller than current one
		if (fontImage == 0)
			return;
		nvgImageSize(ctx, fontImage, &iw, &ih);
		for (i = j = 0; i < ctx->fontImageIdx; i++) {
			if (ctx->fontImages[i] != 0) {
				int nw, nh;
				nvgImageSize(ctx, ctx->fontImages[i], &nw, &nh);
				if (nw < iw || nh < ih)
					nvgDeleteImage(ctx, ctx->fontImages[i]);
				else
					ctx->fontImages[j++] = ctx->fontImages[i];
			}
		}
		// make current font image to first
		ctx->fontImages[j++] = ctx->fontImages[0];
		ctx->fontImages[0] = fontImage;
		ctx->fontImageIdx = 0;
		// clear all images after j
		for (i = j; i < NVG_MAX_FONTIMAGES; i++)
			ctx->fontImages[i] = 0;
	}
}

NVGcolor nvgRGB(unsigned char r, unsigned char g, unsigned char b)
{
	return nvgRGBA(r,g,b,255);
}

NVGcolor nvgRGBf(float r, float g, float b)
{
	return nvgRGBAf(r,g,b,1.0f);
}

NVGcolor nvgRGBA(unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
	NVGcolor color;
	// Use longer initialization to suppress warning.
	color.r = r / 255.0f;
	color.g = g / 255.0f;
	color.b = b / 255.0f;
	color.a = a / 255.0f;
	return color;
}

NVGcolor nvgRGBAf(float r, float g, float b, float a)
{
	NVGcolor color;
	// Use longer initialization to suppress warning.
	color.r = r;
	color.g = g;
	color.b = b;
	color.a = a;
	return color;
}

NVGcolor nvgTransRGBA(NVGcolor c, unsigned char a)
{
	c.a = a / 255.0f;
	return c;
}

NVGcolor nvgTransRGBAf(NVGcolor c, float a)
{
	c.a = a;
	return c;
}

NVGcolor nvgLerpRGBA(NVGcolor c0, NVGcolor c1, float u)
{
	int i;
	float oneminu;
	NVGcolor cint = {{{0}}};

	u = nvg__clampf(u, 0.0f, 1.0f);
	oneminu = 1.0f - u;
	for( i = 0; i <4; i++ )
	{
		cint.rgba[i] = c0.rgba[i] * oneminu + c1.rgba[i] * u;
	}

	return cint;
}

NVGcolor nvgHSL(float h, float s, float l)
{
	return nvgHSLA(h,s,l,255);
}

static float nvg__hue(float h, float m1, float m2)
{
	if (h < 0) h += 1;
	if (h > 1) h -= 1;
	if (h < 1.0f/6.0f)
		return m1 + (m2 - m1) * h * 6.0f;
	else if (h < 3.0f/6.0f)
		return m2;
	else if (h < 4.0f/6.0f)
		return m1 + (m2 - m1) * (2.0f/3.0f - h) * 6.0f;
	return m1;
}

NVGcolor nvgHSLA(float h, float s, float l, unsigned char a)
{
	float m1, m2;
	NVGcolor col;
	h = nvg__modf(h, 1.0f);
	if (h < 0.0f) h += 1.0f;
	s = nvg__clampf(s, 0.0f, 1.0f);
	l = nvg__clampf(l, 0.0f, 1.0f);
	m2 = l <= 0.5f ? (l * (1 + s)) : (l + s - l * s);
	m1 = 2 * l - m2;
	col.r = nvg__clampf(nvg__hue(h + 1.0f/3.0f, m1, m2), 0.0f, 1.0f);
	col.g = nvg__clampf(nvg__hue(h, m1, m2), 0.0f, 1.0f);
	col.b = nvg__clampf(nvg__hue(h - 1.0f/3.0f, m1, m2), 0.0f, 1.0f);
	col.a = a/255.0f;
	return col;
}

void nvgTransformIdentity(float* t)
{
	t[0] = 1.0f; t[1] = 0.0f;
	t[2] = 0.0f; t[3] = 1.0f;
	t[4] = 0.0f; t[5] = 0.0f;
}

void nvgTransformTranslate(float* t, float tx, float ty)
{
	t[0] = 1.0f; t[1] = 0.0f;
	t[2] = 0.0f; t[3] = 1.0f;
	t[4] = tx; t[5] = ty;
}

void nvgTransformScale(float* t, float sx, float sy)
{
	t[0] = sx; t[1] = 0.0f;
	t[2] = 0.0f; t[3] = sy;
	t[4] = 0.0f; t[5] = 0.0f;
}

void nvgTransformRotate(float* t, float a)
{
	float cs = nvg__cosf(a), sn = nvg__sinf(a);
	t[0] = cs; t[1] = sn;
	t[2] = -sn; t[3] = cs;
	t[4] = 0.0f; t[5] = 0.0f;
}

void nvgTransformSkewX(float* t, float a)
{
	t[0] = 1.0f; t[1] = 0.0f;
	t[2] = nvg__tanf(a); t[3] = 1.0f;
	t[4] = 0.0f; t[5] = 0.0f;
}

void nvgTransformSkewY(float* t, float a)
{
	t[0] = 1.0f; t[1] = nvg__tanf(a);
	t[2] = 0.0f; t[3] = 1.0f;
	t[4] = 0.0f; t[5] = 0.0f;
}

void nvgTransformMultiply(float* t, const float* s)
{
	float t0 = t[0] * s[0] + t[1] * s[2];
	float t2 = t[2] * s[0] + t[3] * s[2];
	float t4 = t[4] * s[0] + t[5] * s[2] + s[4];
	t[1] = t[0] * s[1] + t[1] * s[3];
	t[3] = t[2] * s[1] + t[3] * s[3];
	t[5] = t[4] * s[1] + t[5] * s[3] + s[5];
	t[0] = t0;
	t[2] = t2;
	t[4] = t4;
}

void nvgTransformPremultiply(float* t, const float* s)
{
	float s2[6];
	memcpy(s2, s, sizeof(float)*6);
	nvgTransformMultiply(s2, t);
	memcpy(t, s2, sizeof(float)*6);
}

int nvgTransformInverse(float* inv, const float* t)
{
	double invdet, det = (double)t[0] * t[3] - (double)t[2] * t[1];
	if (det > -1e-6 && det < 1e-6) {
		nvgTransformIdentity(inv);
		return 0;
	}
	invdet = 1.0 / det;
	inv[0] = (float)(t[3] * invdet);
	inv[2] = (float)(-t[2] * invdet);
	inv[4] = (float)(((double)t[2] * t[5] - (double)t[3] * t[4]) * invdet);
	inv[1] = (float)(-t[1] * invdet);
	inv[3] = (float)(t[0] * invdet);
	inv[5] = (float)(((double)t[1] * t[4] - (double)t[0] * t[5]) * invdet);
	return 1;
}

void nvgTransformPoint(float* dx, float* dy, const float* t, float sx, float sy)
{
	*dx = sx*t[0] + sy*t[2] + t[4];
	*dy = sx*t[1] + sy*t[3] + t[5];
}

float nvgDegToRad(float deg)
{
	return deg / 180.0f * NVG_PI;
}

float nvgRadToDeg(float rad)
{
	return rad / NVG_PI * 180.0f;
}

static void nvg__setPaintColor(NVGpaint* p, NVGcolor color)
{
	memset(p, 0, sizeof(*p));
	nvgTransformIdentity(p->xform);
	p->radius = 0.0f;
	p->feather = 1.0f;
	p->innerColor = color;
	p->outerColor = color;
}


// State handling
void nvgSave(NVGcontext* ctx)
{
	if (ctx->nstates >= NVG_MAX_STATES)
		return;
	if (ctx->nstates > 0)
		memcpy(&ctx->states[ctx->nstates], &ctx->states[ctx->nstates-1], sizeof(NVGstate));
	ctx->nstates++;
}

void nvgRestore(NVGcontext* ctx)
{
	if (ctx->nstates <= 1)
		return;
	ctx->nstates--;
}

void nvgReset(NVGcontext* ctx)
{
	NVGstate* state = nvg__getState(ctx);
	memset(state, 0, sizeof(*state));

	nvg__setPaintColor(&state->fill, nvgRGBA(255,255,255,255));
	nvg__setPaintColor(&state->stroke, nvgRGBA(0,0,0,255));
	state->compositeOperation = nvg__compositeOperationState(NVG_SOURCE_OVER);
	state->shapeAntiAlias = 1;
	state->strokeWidth = 1.0f;
	state->miterLimit = 10.0f;
	state->lineCap = NVG_BUTT;
	state->lineJoin = NVG_MITER;
	state->alpha = 1.0f;
	nvgTransformIdentity(state->xform);

	state->scissor.extent[0] = -1.0f;
	state->scissor.extent[1] = -1.0f;

	state->fontSize = 16.0f;
	state->letterSpacing = 0.0f;
	state->lineHeight = 1.0f;
	state->fontBlur = 0.0f;
	state->textAlign = NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE;
	state->fontId = 0;
}

// State setting
void nvgShapeAntiAlias(NVGcontext* ctx, int enabled)
{
	NVGstate* state = nvg__getState(ctx);
	state->shapeAntiAlias = enabled;
}

void nvgStrokeWidth(NVGcontext* ctx, float width)
{
	NVGstate* state = nvg__getState(ctx);
	state->strokeWidth = width;
}

void nvgMiterLimit(NVGcontext* ctx, float limit)
{
	NVGstate* state = nvg__getState(ctx);
	state->miterLimit = limit;
}

void nvgLineCap(NVGcontext* ctx, int cap)
{
	NVGstate* state = nvg__getState(ctx);
	state->lineCap = cap;
}

void nvgLineJoin(NVGcontext* ctx, int join)
{
	NVGstate* state = nvg__getState(ctx);
	state->lineJoin = join;
}

void nvgGlobalAlpha(NVGcontext* ctx, float alpha)
{
	NVGstate* state = nvg__getState(ctx);
	state->alpha = alpha;
}

void nvgTransform(NVGcontext* ctx, float a, float b, float c, float d, float e, float f)
{
	NVGstate* state = nvg__getState(ctx);
	float t[6] = { a, b, c, d, e, f };
	nvgTransformPremultiply(state->xform, t);
}

void nvgResetTransform(NVGcontext* ctx)
{
	NVGstate* state = nvg__getState(ctx);
	nvgTransformIdentity(state->xform);
}

void nvgTranslate(NVGcontext* ctx, float x, float y)
{
	NVGstate* state = nvg__getState(ctx);
	float t[6];
	nvgTransformTranslate(t, x,y);
	nvgTransformPremultiply(state->xform, t);
}

void nvgRotate(NVGcontext* ctx, float angle)
{
	NVGstate* state = nvg__getState(ctx);
	float t[6];
	nvgTransformRotate(t, angle);
	nvgTransformPremultiply(state->xform, t);
}

void nvgSkewX(NVGcontext* ctx, float angle)
{
	NVGstate* state = nvg__getState(ctx);
	float t[6];
	nvgTransformSkewX(t, angle);
	nvgTransformPremultiply(state->xform, t);
}

void nvgSkewY(NVGcontext* ctx, float angle)
{
	NVGstate* state = nvg__getState(ctx);
	float t[6];
	nvgTransformSkewY(t, angle);
	nvgTransformPremultiply(state->xform, t);
}

void nvgScale(NVGcontext* ctx, float x, float y)
{
	NVGstate* state = nvg__getState(ctx);
	float t[6];
	nvgTransformScale(t, x,y);
	nvgTransformPremultiply(state->xform, t);
}

void nvgCurrentTransform(NVGcontext* ctx, float* xform)
{
	NVGstate* state = nvg__getState(ctx);
	if (xform == NULL) return;
	memcpy(xform, state->xform, sizeof(float)*6);
}

void nvgStrokeColor(NVGcontext* ctx, NVGcolor color)
{
	NVGstate* state = nvg__getState(ctx);
	nvg__setPaintColor(&state->stroke, color);
}

void nvgStrokePaint(NVGcontext* ctx, NVGpaint paint)
{
	NVGstate* state = nvg__getState(ctx);
	state->stroke = paint;
	nvgTransformMultiply(state->stroke.xform, state->xform);
}

void nvgFillColor(NVGcontext* ctx, NVGcolor color)
{
	NVGstate* state = nvg__getState(ctx);
	nvg__setPaintColor(&state->fill, color);
}

void nvgFillPaint(NVGcontext* ctx, NVGpaint paint)
{
	NVGstate* state = nvg__getState(ctx);
	state->fill = paint;
	nvgTransformMultiply(state->fill.xform, state->xform);
}

int nvgCreateImage(NVGcontext* ctx, const char* filename, int imageFlags)
{
	int w, h, n, image;
	unsigned char* img;
	stbi_set_unpremultiply_on_load(1);
	stbi_convert_iphone_png_to_rgb(1);
	img = stbi_load(filename, &w, &h, &n, 4);
	if (img == NULL) {
//		printf("Failed to load %s - %s\n", filename, stbi_failure_reason());
		return 0;
	}
	image = nvgCreateImageRGBA(ctx, w, h, imageFlags, img);
	stbi_image_free(img);
	return image;
}

int nvgCreateImageMem(NVGcontext* ctx, int imageFlags, unsigned char* data, int ndata)
{
	int w, h, n, image;
	unsigned char* img = stbi_load_from_memory(data, ndata, &w, &h, &n, 4);
	if (img == NULL) {
//		printf("Failed to load %s - %s\n", filename, stbi_failure_reason());
		return 0;
	}
	image = nvgCreateImageRGBA(ctx, w, h, imageFlags, img);
	stbi_image_free(img);
	return image;
}

int nvgCreateImageRGBA(NVGcontext* ctx, int w, int h, int imageFlags, const unsigned char* data)
{
	return ctx->params.renderCreateTexture(ctx->params.userPtr, NVG_TEXTURE_RGBA, w, h, imageFlags, data);
}

void nvgUpdateImage(NVGcontext* ctx, int image, const unsigned char* data)
{
	int w, h;
	ctx->params.renderGetTextureSize(ctx->params.userPtr, image, &w, &h);
	ctx->params.renderUpdateTexture(ctx->params.userPtr, image, 0,0, w,h, data);
}

void nvgImageSize(NVGcontext* ctx, int image, int* w, int* h)
{
	ctx->params.renderGetTextureSize(ctx->params.userPtr, image, w, h);
}

void nvgDeleteImage(NVGcontext* ctx, int image)
{
	ctx->params.renderDeleteTexture(ctx->params.userPtr, image);
}

NVGpaint nvgLinearGradient(NVGcontext* ctx,
								  float sx, float sy, float ex, float ey,
								  NVGcolor icol, NVGcolor ocol)
{
	NVGpaint p;
	float dx, dy, d;
	const float large = 1e5;
	NVG_NOTUSED(ctx);
	memset(&p, 0, sizeof(p));

	// Calculate transform aligned to the line
	dx = ex - sx;
	dy = ey - sy;
	d = sqrtf(dx*dx + dy*dy);
	if (d > 0.0001f) {
		dx /= d;
		dy /= d;
	} else {
		dx = 0;
		dy = 1;
	}

	p.xform[0] = dy; p.xform[1] = -dx;
	p.xform[2] = dx; p.xform[3] = dy;
	p.xform[4] = sx - dx*large; p.xform[5] = sy - dy*large;

	p.extent[0] = large;
	p.extent[1] = large + d*0.5f;

	p.radius = 0.0f;

	p.feather = nvg__maxf(1.0f, d);

	p.innerColor = icol;
	p.outerColor = ocol;

	return p;
}

NVGpaint nvgRadialGradient(NVGcontext* ctx,
								  float cx, float cy, float inr, float outr,
								  NVGcolor icol, NVGcolor ocol)
{
	NVGpaint p;
	float r = (inr+outr)*0.5f;
	float f = (outr-inr);
	NVG_NOTUSED(ctx);
	memset(&p, 0, sizeof(p));

	nvgTransformIdentity(p.xform);
	p.xform[4] = cx;
	p.xform[5] = cy;

	p.extent[0] = r;
	p.extent[1] = r;

	p.radius = r;

	p.feather = nvg__maxf(1.0f, f);

	p.innerColor = icol;
	p.outerColor = ocol;

	return p;
}

NVGpaint nvgBoxGradient(NVGcontext* ctx,
							   float x, float y, float w, float h, float r, float f,
							   NVGcolor icol, NVGcolor ocol)
{
	NVGpaint p;
	NVG_NOTUSED(ctx);
	memset(&p, 0, sizeof(p));

	nvgTransformIdentity(p.xform);
	p.xform[4] = x+w*0.5f;
	p.xform[5] = y+h*0.5f;

	p.extent[0] = w*0.5f;
	p.extent[1] = h*0.5f;

	p.radius = r;

	p.feather = nvg__maxf(1.0f, f);

	p.innerColor = icol;
	p.outerColor = ocol;

	return p;
}


NVGpaint nvgImagePattern(NVGcontext* ctx,
								float cx, float cy, float w, float h, float angle,
								int image, float alpha)
{
	NVGpaint p;
	NVG_NOTUSED(ctx);
	memset(&p, 0, sizeof(p));

	nvgTransformRotate(p.xform, angle);
	p.xform[4] = cx;
	p.xform[5] = cy;

	p.extent[0] = w;
	p.extent[1] = h;

	p.image = image;

	p.innerColor = p.outerColor = nvgRGBAf(1,1,1,alpha);

	return p;
}

// Scissoring
void nvgScissor(NVGcontext* ctx, float x, float y, float w, float h)
{
	NVGstate* state = nvg__getState(ctx);

	w = nvg__maxf(0.0f, w);
	h = nvg__maxf(0.0f, h);

	nvgTransformIdentity(state->scissor.xform);
	state->scissor.xform[4] = x+w*0.5f;
	state->scissor.xform[5] = y+h*0.5f;
	nvgTransformMultiply(state->scissor.xform, state->xform);

	state->scissor.extent[0] = w*0.5f;
	state->scissor.extent[1] = h*0.5f;
}

static void nvg__isectRects(float* dst,
							float ax, float ay, float aw, float ah,
							float bx, float by, float bw, float bh)
{
	float minx = nvg__maxf(ax, bx);
	float miny = nvg__maxf(ay, by);
	float maxx = nvg__minf(ax+aw, bx+bw);
	float maxy = nvg__minf(ay+ah, by+bh);
	dst[0] = minx;
	dst[1] = miny;
	dst[2] = nvg__maxf(0.0f, maxx - minx);
	dst[3] = nvg__maxf(0.0f, maxy - miny);
}

void nvgIntersectScissor(NVGcontext* ctx, float x, float y, float w, float h)
{
	NVGstate* state = nvg__getState(ctx);
	float pxform[6], invxorm[6];
	float rect[4];
	float ex, ey, tex, tey;

	// If no previous scissor has been set, set the scissor as current scissor.
	if (state->scissor.extent[0] < 0) {
		nvgScissor(ctx, x, y, w, h);
		return;
	}

	// Transform the current scissor rect into current transform space.
	// If there is difference in rotation, this will be approximation.
	memcpy(pxform, state->scissor.xform, sizeof(float)*6);
	ex = state->scissor.extent[0];
	ey = state->scissor.extent[1];
	nvgTransformInverse(invxorm, state->xform);
	nvgTransformMultiply(pxform, invxorm);
	tex = ex*nvg__absf(pxform[0]) + ey*nvg__absf(pxform[2]);
	tey = ex*nvg__absf(pxform[1]) + ey*nvg__absf(pxform[3]);

	// Intersect rects.
	nvg__isectRects(rect, pxform[4]-tex,pxform[5]-tey,tex*2,tey*2, x,y,w,h);

	nvgScissor(ctx, rect[0], rect[1], rect[2], rect[3]);
}

void nvgResetScissor(NVGcontext* ctx)
{
	NVGstate* state = nvg__getState(ctx);
	memset(state->scissor.xform, 0, sizeof(state->scissor.xform));
	state->scissor.extent[0] = -1.0f;
	state->scissor.extent[1] = -1.0f;
}

// Global composite operation.
void nvgGlobalCompositeOperation(NVGcontext* ctx, int op)
{
	NVGstate* state = nvg__getState(ctx);
	state->compositeOperation = nvg__compositeOperationState(op);
}

void nvgGlobalCompositeBlendFunc(NVGcontext* ctx, int sfactor, int dfactor)
{
	nvgGlobalCompositeBlendFuncSeparate(ctx, sfactor, dfactor, sfactor, dfactor);
}

void nvgGlobalCompositeBlendFuncSeparate(NVGcontext* ctx, int srcRGB, int dstRGB, int srcAlpha, int dstAlpha)
{
	NVGcompositeOperationState op;
	op.srcRGB = srcRGB;
	op.dstRGB = dstRGB;
	op.srcAlpha = srcAlpha;
	op.dstAlpha = dstAlpha;

	NVGstate* state = nvg__getState(ctx);
	state->compositeOperation = op;
}

static int nvg__ptEquals(float x1, float y1, float x2, float y2, float tol)
{
	float dx = x2 - x1;
	float dy = y2 - y1;
	return dx*dx + dy*dy < tol*tol;
}

static float nvg__distPtSeg(float x, float y, float px, float py, float qx, float qy)
{
	float pqx, pqy, dx, dy, d, t;
	pqx = qx-px;
	pqy = qy-py;
	dx = x-px;
	dy = y-py;
	d = pqx*pqx + pqy*pqy;
	t = pqx*dx + pqy*dy;
	if (d > 0) t /= d;
	if (t < 0) t = 0;
	else if (t > 1) t = 1;
	dx = px + t*pqx - x;
	dy = py + t*pqy - y;
	return dx*dx + dy*dy;
}

static void nvg__appendCommands(NVGcontext* ctx, float* vals, int nvals)
{
	NVGstate* state = nvg__getState(ctx);
	int i;

	if (ctx->ncommands+nvals > ctx->ccommands) {
		float* commands;
		int ccommands = ctx->ncommands+nvals + ctx->ccommands/2;
		commands = (float*)realloc(ctx->commands, sizeof(float)*ccommands);
		if (commands == NULL) return;
		ctx->commands = commands;
		ctx->ccommands = ccommands;
	}

	if ((int)vals[0] != NVG_CLOSE && (int)vals[0] != NVG_WINDING) {
		ctx->commandx = vals[nvals-2];
		ctx->commandy = vals[nvals-1];
	}

	// transform commands
	i = 0;
	while (i < nvals) {
		int cmd = (int)vals[i];
		switch (cmd) {
		case NVG_MOVETO:
			nvgTransformPoint(&vals[i+1],&vals[i+2], state->xform, vals[i+1],vals[i+2]);
			i += 3;
			break;
		case NVG_LINETO:
			nvgTransformPoint(&vals[i+1],&vals[i+2], state->xform, vals[i+1],vals[i+2]);
			i += 3;
			break;
		case NVG_BEZIERTO:
			nvgTransformPoint(&vals[i+1],&vals[i+2], state->xform, vals[i+1],vals[i+2]);
			nvgTransformPoint(&vals[i+3],&vals[i+4], state->xform, vals[i+3],vals[i+4]);
			nvgTransformPoint(&vals[i+5],&vals[i+6], state->xform, vals[i+5],vals[i+6]);
			i += 7;
			break;
		case NVG_CLOSE:
			i++;
			break;
		case NVG_WINDING:
			i += 2;
			break;
		default:
			i++;
		}
	}

	memcpy(&ctx->commands[ctx->ncommands], vals, nvals*sizeof(float));

	ctx->ncommands += nvals;
}


static void nvg__clearPathCache(NVGcontext* ctx)
{
	ctx->cache->npoints = 0;
	ctx->cache->npaths = 0;
}

static NVGpath* nvg__lastPath(NVGcontext* ctx)
{
	if (ctx->cache->npaths > 0)
		return &ctx->cache->paths[ctx->cache->npaths-1];
	return NULL;
}

static void nvg__addPath(NVGcontext* ctx)
{
	NVGpath* path;
	if (ctx->cache->npaths+1 > ctx->cache->cpaths) {
		NVGpath* paths;
		int cpaths = ctx->cache->npaths+1 + ctx->cache->cpaths/2;
		paths = (NVGpath*)realloc(ctx->cache->paths, sizeof(NVGpath)*cpaths);
		if (paths == NULL) return;
		ctx->cache->paths = paths;
		ctx->cache->cpaths = cpaths;
	}
	path = &ctx->cache->paths[ctx->cache->npaths];
	memset(path, 0, sizeof(*path));
	path->first = ctx->cache->npoints;
	path->winding = NVG_CCW;

	ctx->cache->npaths++;
}

static NVGpoint* nvg__lastPoint(NVGcontext* ctx)
{
	if (ctx->cache->npoints > 0)
		return &ctx->cache->points[ctx->cache->npoints-1];
	return NULL;
}

static void nvg__addPoint(NVGcontext* ctx, float x, float y, int flags)
{
	NVGpath* path = nvg__lastPath(ctx);
	NVGpoint* pt;
	if (path == NULL) return;

	if (path->count > 0 && ctx->cache->npoints > 0) {
		pt = nvg__lastPoint(ctx);
		if (nvg__ptEquals(pt->x,pt->y, x,y, ctx->distTol)) {
			pt->flags |= flags;
			return;
		}
	}

	if (ctx->cache->npoints+1 > ctx->cache->cpoints) {
		NVGpoint* points;
		int cpoints = ctx->cache->npoints+1 + ctx->cache->cpoints/2;
		points = (NVGpoint*)realloc(ctx->cache->points, sizeof(NVGpoint)*cpoints);
		if (points == NULL) return;
		ctx->cache->points = points;
		ctx->cache->cpoints = cpoints;
	}

	pt = &ctx->cache->points[ctx->cache->npoints];
	memset(pt, 0, sizeof(*pt));
	pt->x = x;
	pt->y = y;
	pt->flags = (unsigned char)flags;

	ctx->cache->npoints++;
	path->count++;
}

static void nvg__closePath(NVGcontext* ctx)
{
	NVGpath* path = nvg__lastPath(ctx);
	if (path == NULL) return;
	path->closed = 1;
}

static void nvg__pathWinding(NVGcontext* ctx, int winding)
{
	NVGpath* path = nvg__lastPath(ctx);
	if (path == NULL) return;
	path->winding = winding;
}

static float nvg__getAverageScale(float *t)
{
	float sx = sqrtf(t[0]*t[0] + t[2]*t[2]);
	float sy = sqrtf(t[1]*t[1] + t[3]*t[3]);
	return (sx + sy) * 0.5f;
}

static NVGvertex* nvg__allocTempVerts(NVGcontext* ctx, int nverts)
{
	if (nverts > ctx->cache->cverts) {
		NVGvertex* verts;
		int cverts = (nverts + 0xff) & ~0xff; // Round up to prevent allocations when things change just slightly.
		verts = (NVGvertex*)realloc(ctx->cache->verts, sizeof(NVGvertex)*cverts);
		if (verts == NULL) return NULL;
		ctx->cache->verts = verts;
		ctx->cache->cverts = cverts;
	}

	return ctx->cache->verts;
}

static float nvg__triarea2(float ax, float ay, float bx, float by, float cx, float cy)
{
	float abx = bx - ax;
	float aby = by - ay;
	float acx = cx - ax;
	float acy = cy - ay;
	return acx*aby - abx*acy;
}

static float nvg__polyArea(NVGpoint* pts, int npts)
{
	int i;
	float area = 0;
	for (i = 2; i < npts; i++) {
		NVGpoint* a = &pts[0];
		NVGpoint* b = &pts[i-1];
		NVGpoint* c = &pts[i];
		area += nvg__triarea2(a->x,a->y, b->x,b->y, c->x,c->y);
	}
	return area * 0.5f;
}

static void nvg__polyReverse(NVGpoint* pts, int npts)
{
	NVGpoint tmp;
	int i = 0, j = npts-1;
	while (i < j) {
		tmp = pts[i];
		pts[i] = pts[j];
		pts[j] = tmp;
		i++;
		j--;
	}
}


static void nvg__vset(NVGvertex* vtx, float x, float y, float u, float v)
{
	vtx->x = x;
	vtx->y = y;
	vtx->u = u;
	vtx->v = v;
}

static void nvg__tesselateBezier(NVGcontext* ctx,
								 float x1, float y1, float x2, float y2,
								 float x3, float y3, float x4, float y4,
								 int level, int type)
{
	float x12,y12,x23,y23,x34,y34,x123,y123,x234,y234,x1234,y1234;
	float dx,dy,d2,d3;

	if (level > 10) return;

	x12 = (x1+x2)*0.5f;
	y12 = (y1+y2)*0.5f;
	x23 = (x2+x3)*0.5f;
	y23 = (y2+y3)*0.5f;
	x34 = (x3+x4)*0.5f;
	y34 = (y3+y4)*0.5f;
	x123 = (x12+x23)*0.5f;
	y123 = (y12+y23)*0.5f;

	dx = x4 - x1;
	dy = y4 - y1;
	d2 = nvg__absf(((x2 - x4) * dy - (y2 - y4) * dx));
	d3 = nvg__absf(((x3 - x4) * dy - (y3 - y4) * dx));

	if ((d2 + d3)*(d2 + d3) < ctx->tessTol * (dx*dx + dy*dy)) {
		nvg__addPoint(ctx, x4, y4, type);
		return;
	}

/*	if (nvg__absf(x1+x3-x2-x2) + nvg__absf(y1+y3-y2-y2) + nvg__absf(x2+x4-x3-x3) + nvg__absf(y2+y4-y3-y3) < ctx->tessTol) {
		nvg__addPoint(ctx, x4, y4, type);
		return;
	}*/

	x234 = (x23+x34)*0.5f;
	y234 = (y23+y34)*0.5f;
	x1234 = (x123+x234)*0.5f;
	y1234 = (y123+y234)*0.5f;

	nvg__tesselateBezier(ctx, x1,y1, x12,y12, x123,y123, x1234,y1234, level+1, 0);
	nvg__tesselateBezier(ctx, x1234,y1234, x234,y234, x34,y34, x4,y4, level+1, type);
}

static void nvg__flattenPaths(NVGcontext* ctx)
{
	NVGpathCache* cache = ctx->cache;
//	NVGstate* state = nvg__getState(ctx);
	NVGpoint* last;
	NVGpoint* p0;
	NVGpoint* p1;
	NVGpoint* pts;
	NVGpath* path;
	int i, j;
	float* cp1;
	float* cp2;
	float* p;
	float area;

	if (cache->npaths > 0)
		return;

	// Flatten
	i = 0;
	while (i < ctx->ncommands) {
		int cmd = (int)ctx->commands[i];
		switch (cmd) {
		case NVG_MOVETO:
			nvg__addPath(ctx);
			p = &ctx->commands[i+1];
			nvg__addPoint(ctx, p[0], p[1], NVG_PT_CORNER);
			i += 3;
			break;
		case NVG_LINETO:
			p = &ctx->commands[i+1];
			nvg__addPoint(ctx, p[0], p[1], NVG_PT_CORNER);
			i += 3;
			break;
		case NVG_BEZIERTO:
			last = nvg__lastPoint(ctx);
			if (last != NULL) {
				cp1 = &ctx->commands[i+1];
				cp2 = &ctx->commands[i+3];
				p = &ctx->commands[i+5];
				nvg__tesselateBezier(ctx, last->x,last->y, cp1[0],cp1[1], cp2[0],cp2[1], p[0],p[1], 0, NVG_PT_CORNER);
			}
			i += 7;
			break;
		case NVG_CLOSE:
			nvg__closePath(ctx);
			i++;
			break;
		case NVG_WINDING:
			nvg__pathWinding(ctx, (int)ctx->commands[i+1]);
			i += 2;
			break;
		default:
			i++;
		}
	}

	cache->bounds[0] = cache->bounds[1] = 1e6f;
	cache->bounds[2] = cache->bounds[3] = -1e6f;

	// Calculate the direction and length of line segments.
	for (j = 0; j < cache->npaths; j++) {
		path = &cache->paths[j];
		pts = &cache->points[path->first];

		// If the first and last points are the same, remove the last, mark as closed path.
		p0 = &pts[path->count-1];
		p1 = &pts[0];
		if (nvg__ptEquals(p0->x,p0->y, p1->x,p1->y, ctx->distTol)) {
			path->count--;
			p0 = &pts[path->count-1];
			path->closed = 1;
		}

		// Enforce winding.
		if (path->count > 2) {
			area = nvg__polyArea(pts, path->count);
			if (path->winding == NVG_CCW && area < 0.0f)
				nvg__polyReverse(pts, path->count);
			if (path->winding == NVG_CW && area > 0.0f)
				nvg__polyReverse(pts, path->count);
		}

		for(i = 0; i < path->count; i++) {
			// Calculate segment direction and length
			p0->dx = p1->x - p0->x;
			p0->dy = p1->y - p0->y;
			p0->len = nvg__normalize(&p0->dx, &p0->dy);
			// Update bounds
			cache->bounds[0] = nvg__minf(cache->bounds[0], p0->x);
			cache->bounds[1] = nvg__minf(cache->bounds[1], p0->y);
			cache->bounds[2] = nvg__maxf(cache->bounds[2], p0->x);
			cache->bounds[3] = nvg__maxf(cache->bounds[3], p0->y);
			// Advance
			p0 = p1++;
		}
	}
}

static int nvg__curveDivs(float r, float arc, float tol)
{
	float da = acosf(r / (r + tol)) * 2.0f;
	return nvg__maxi(2, (int)ceilf(arc / da));
}

static void nvg__chooseBevel(int bevel, NVGpoint* p0, NVGpoint* p1, float w,
							float* x0, float* y0, float* x1, float* y1)
{
	if (bevel) {
		*x0 = p1->x + p0->dy * w;
		*y0 = p1->y - p0->dx * w;
		*x1 = p1->x + p1->dy * w;
		*y1 = p1->y - p1->dx * w;
	} else {
		*x0 = p1->x + p1->dmx * w;
		*y0 = p1->y + p1->dmy * w;
		*x1 = p1->x + p1->dmx * w;
		*y1 = p1->y + p1->dmy * w;
	}
}

static NVGvertex* nvg__roundJoin(NVGvertex* dst, NVGpoint* p0, NVGpoint* p1,
								 float lw, float rw, float lu, float ru, int ncap,
								 float fringe)
{
	int i, n;
	float dlx0 = p0->dy;
	float dly0 = -p0->dx;
	float dlx1 = p1->dy;
	float dly1 = -p1->dx;
	NVG_NOTUSED(fringe);

	if (p1->flags & NVG_PT_LEFT) {
		float lx0,ly0,lx1,ly1,a0,a1;
		nvg__chooseBevel(p1->flags & NVG_PR_INNERBEVEL, p0, p1, lw, &lx0,&ly0, &lx1,&ly1);
		a0 = atan2f(-dly0, -dlx0);
		a1 = atan2f(-dly1, -dlx1);
		if (a1 > a0) a1 -= NVG_PI*2;

		nvg__vset(dst, lx0, ly0, lu,1); dst++;
		nvg__vset(dst, p1->x - dlx0*rw, p1->y - dly0*rw, ru,1); dst++;

		n = nvg__clampi((int)ceilf(((a0 - a1) / NVG_PI) * ncap), 2, ncap);
		for (i = 0; i < n; i++) {
			float u = i/(float)(n-1);
			float a = a0 + u*(a1-a0);
			float rx = p1->x + cosf(a) * rw;
			float ry = p1->y + sinf(a) * rw;
			nvg__vset(dst, p1->x, p1->y, 0.5f,1); dst++;
			nvg__vset(dst, rx, ry, ru,1); dst++;
		}

		nvg__vset(dst, lx1, ly1, lu,1); dst++;
		nvg__vset(dst, p1->x - dlx1*rw, p1->y - dly1*rw, ru,1); dst++;

	} else {
		float rx0,ry0,rx1,ry1,a0,a1;
		nvg__chooseBevel(p1->flags & NVG_PR_INNERBEVEL, p0, p1, -rw, &rx0,&ry0, &rx1,&ry1);
		a0 = atan2f(dly0, dlx0);
		a1 = atan2f(dly1, dlx1);
		if (a1 < a0) a1 += NVG_PI*2;

		nvg__vset(dst, p1->x + dlx0*rw, p1->y + dly0*rw, lu,1); dst++;
		nvg__vset(dst, rx0, ry0, ru,1); dst++;

		n = nvg__clampi((int)ceilf(((a1 - a0) / NVG_PI) * ncap), 2, ncap);
		for (i = 0; i < n; i++) {
			float u = i/(float)(n-1);
			float a = a0 + u*(a1-a0);
			float lx = p1->x + cosf(a) * lw;
			float ly = p1->y + sinf(a) * lw;
			nvg__vset(dst, lx, ly, lu,1); dst++;
			nvg__vset(dst, p1->x, p1->y, 0.5f,1); dst++;
		}

		nvg__vset(dst, p1->x + dlx1*rw, p1->y + dly1*rw, lu,1); dst++;
		nvg__vset(dst, rx1, ry1, ru,1); dst++;

	}
	return dst;
}

static NVGvertex* nvg__bevelJoin(NVGvertex* dst, NVGpoint* p0, NVGpoint* p1,
										float lw, float rw, float lu, float ru, float fringe)
{
	float rx0,ry0,rx1,ry1;
	float lx0,ly0,lx1,ly1;
	float dlx0 = p0->dy;
	float dly0 = -p0->dx;
	float dlx1 = p1->dy;
	float dly1 = -p1->dx;
	NVG_NOTUSED(fringe);

	if (p1->flags & NVG_PT_LEFT) {
		nvg__chooseBevel(p1->flags & NVG_PR_INNERBEVEL, p0, p1, lw, &lx0,&ly0, &lx1,&ly1);

		nvg__vset(dst, lx0, ly0, lu,1); dst++;
		nvg__vset(dst, p1->x - dlx0*rw, p1->y - dly0*rw, ru,1); dst++;

		if (p1->flags & NVG_PT_BEVEL) {
			nvg__vset(dst, lx0, ly0, lu,1); dst++;
			nvg__vset(dst, p1->x - dlx0*rw, p1->y - dly0*rw, ru,1); dst++;

			nvg__vset(dst, lx1, ly1, lu,1); dst++;
			nvg__vset(dst, p1->x - dlx1*rw, p1->y - dly1*rw, ru,1); dst++;
		} else {
			rx0 = p1->x - p1->dmx * rw;
			ry0 = p1->y - p1->dmy * rw;

			nvg__vset(dst, p1->x, p1->y, 0.5f,1); dst++;
			nvg__vset(dst, p1->x - dlx0*rw, p1->y - dly0*rw, ru,1); dst++;

			nvg__vset(dst, rx0, ry0, ru,1); dst++;
			nvg__vset(dst, rx0, ry0, ru,1); dst++;

			nvg__vset(dst, p1->x, p1->y, 0.5f,1); dst++;
			nvg__vset(dst, p1->x - dlx1*rw, p1->y - dly1*rw, ru,1); dst++;
		}

		nvg__vset(dst, lx1, ly1, lu,1); dst++;
		nvg__vset(dst, p1->x - dlx1*rw, p1->y - dly1*rw, ru,1); dst++;

	} else {
		nvg__chooseBevel(p1->flags & NVG_PR_INNERBEVEL, p0, p1, -rw, &rx0,&ry0, &rx1,&ry1);

		nvg__vset(dst, p1->x + dlx0*lw, p1->y + dly0*lw, lu,1); dst++;
		nvg__vset(dst, rx0, ry0, ru,1); dst++;

		if (p1->flags & NVG_PT_BEVEL) {
			nvg__vset(dst, p1->x + dlx0*lw, p1->y + dly0*lw, lu,1); dst++;
			nvg__vset(dst, rx0, ry0, ru,1); dst++;

			nvg__vset(dst, p1->x + dlx1*lw, p1->y + dly1*lw, lu,1); dst++;
			nvg__vset(dst, rx1, ry1, ru,1); dst++;
		} else {
			lx0 = p1->x + p1->dmx * lw;
			ly0 = p1->y + p1->dmy * lw;

			nvg__vset(dst, p1->x + dlx0*lw, p1->y + dly0*lw, lu,1); dst++;
			nvg__vset(dst, p1->x, p1->y, 0.5f,1); dst++;

			nvg__vset(dst, lx0, ly0, lu,1); dst++;
			nvg__vset(dst, lx0, ly0, lu,1); dst++;

			nvg__vset(dst, p1->x + dlx1*lw, p1->y + dly1*lw, lu,1); dst++;
			nvg__vset(dst, p1->x, p1->y, 0.5f,1); dst++;
		}

		nvg__vset(dst, p1->x + dlx1*lw, p1->y + dly1*lw, lu,1); dst++;
		nvg__vset(dst, rx1, ry1, ru,1); dst++;
	}

	return dst;
}

static NVGvertex* nvg__buttCapStart(NVGvertex* dst, NVGpoint* p,
									float dx, float dy, float w, float d,
									float aa, float u0, float u1)
{
	float px = p->x - dx*d;
	float py = p->y - dy*d;
	float dlx = dy;
	float dly = -dx;
	nvg__vset(dst, px + dlx*w - dx*aa, py + dly*w - dy*aa, u0,0); dst++;
	nvg__vset(dst, px - dlx*w - dx*aa, py - dly*w - dy*aa, u1,0); dst++;
	nvg__vset(dst, px + dlx*w, py + dly*w, u0,1); dst++;
	nvg__vset(dst, px - dlx*w, py - dly*w, u1,1); dst++;
	return dst;
}

static NVGvertex* nvg__buttCapEnd(NVGvertex* dst, NVGpoint* p,
								  float dx, float dy, float w, float d,
								  float aa, float u0, float u1)
{
	float px = p->x + dx*d;
	float py = p->y + dy*d;
	float dlx = dy;
	float dly = -dx;
	nvg__vset(dst, px + dlx*w, py + dly*w, u0,1); dst++;
	nvg__vset(dst, px - dlx*w, py - dly*w, u1,1); dst++;
	nvg__vset(dst, px + dlx*w + dx*aa, py + dly*w + dy*aa, u0,0); dst++;
	nvg__vset(dst, px - dlx*w + dx*aa, py - dly*w + dy*aa, u1,0); dst++;
	return dst;
}


static NVGvertex* nvg__roundCapStart(NVGvertex* dst, NVGpoint* p,
									 float dx, float dy, float w, int ncap,
									 float aa, float u0, float u1)
{
	int i;
	float px = p->x;
	float py = p->y;
	float dlx = dy;
	float dly = -dx;
	NVG_NOTUSED(aa);
	for (i = 0; i < ncap; i++) {
		float a = i/(float)(ncap-1)*NVG_PI;
		float ax = cosf(a) * w, ay = sinf(a) * w;
		nvg__vset(dst, px - dlx*ax - dx*ay, py - dly*ax - dy*ay, u0,1); dst++;
		nvg__vset(dst, px, py, 0.5f,1); dst++;
	}
	nvg__vset(dst, px + dlx*w, py + dly*w, u0,1); dst++;
	nvg__vset(dst, px - dlx*w, py - dly*w, u1,1); dst++;
	return dst;
}

static NVGvertex* nvg__roundCapEnd(NVGvertex* dst, NVGpoint* p,
								   float dx, float dy, float w, int ncap,
								   float aa, float u0, float u1)
{
	int i;
	float px = p->x;
	float py = p->y;
	float dlx = dy;
	float dly = -dx;
	NVG_NOTUSED(aa);
	nvg__vset(dst, px + dlx*w, py + dly*w, u0,1); dst++;
	nvg__vset(dst, px - dlx*w, py - dly*w, u1,1); dst++;
	for (i = 0; i < ncap; i++) {
		float a = i/(float)(ncap-1)*NVG_PI;
		float ax = cosf(a) * w, ay = sinf(a) * w;
		nvg__vset(dst, px, py, 0.5f,1); dst++;
		nvg__vset(dst, px - dlx*ax + dx*ay, py - dly*ax + dy*ay, u0,1); dst++;
	}
	return dst;
}


static void nvg__calculateJoins(NVGcontext* ctx, float w, int lineJoin, float miterLimit)
{
	NVGpathCache* cache = ctx->cache;
	int i, j;
	float iw = 0.0f;

	if (w > 0.0f) iw = 1.0f / w;

	// Calculate which joins needs extra vertices to append, and gather vertex count.
	for (i = 0; i < cache->npaths; i++) {
		NVGpath* path = &cache->paths[i];
		NVGpoint* pts = &cache->points[path->first];
		NVGpoint* p0 = &pts[path->count-1];
		NVGpoint* p1 = &pts[0];
		int nleft = 0;

		path->nbevel = 0;

		for (j = 0; j < path->count; j++) {
			float dlx0, dly0, dlx1, dly1, dmr2, cross, limit;
			dlx0 = p0->dy;
			dly0 = -p0->dx;
			dlx1 = p1->dy;
			dly1 = -p1->dx;
			// Calculate extrusions
			p1->dmx = (dlx0 + dlx1) * 0.5f;
			p1->dmy = (dly0 + dly1) * 0.5f;
			dmr2 = p1->dmx*p1->dmx + p1->dmy*p1->dmy;
			if (dmr2 > 0.000001f) {
				float scale = 1.0f / dmr2;
				if (scale > 600.0f) {
					scale = 600.0f;
				}
				p1->dmx *= scale;
				p1->dmy *= scale;
			}

			// Clear flags, but keep the corner.
			p1->flags = (p1->flags & NVG_PT_CORNER) ? NVG_PT_CORNER : 0;

			// Keep track of left turns.
			cross = p1->dx * p0->dy - p0->dx * p1->dy;
			if (cross > 0.0f) {
				nleft++;
				p1->flags |= NVG_PT_LEFT;
			}

			// Calculate if we should use bevel or miter for inner join.
			limit = nvg__maxf(1.01f, nvg__minf(p0->len, p1->len) * iw);
			if ((dmr2 * limit*limit) < 1.0f)
				p1->flags |= NVG_PR_INNERBEVEL;

			// Check to see if the corner needs to be beveled.
			if (p1->flags & NVG_PT_CORNER) {
				if ((dmr2 * miterLimit*miterLimit) < 1.0f || lineJoin == NVG_BEVEL || lineJoin == NVG_ROUND) {
					p1->flags |= NVG_PT_BEVEL;
				}
			}

			if ((p1->flags & (NVG_PT_BEVEL | NVG_PR_INNERBEVEL)) != 0)
				path->nbevel++;

			p0 = p1++;
		}

		path->convex = (nleft == path->count) ? 1 : 0;
	}
}


static int nvg__expandStroke(NVGcontext* ctx, float w, float fringe, int lineCap, int lineJoin, float miterLimit)
{
	NVGpathCache* cache = ctx->cache;
	NVGvertex* verts;
	NVGvertex* dst;
	int cverts, i, j;
	float aa = fringe;//ctx->fringeWidth;
	float u0 = 0.0f, u1 = 1.0f;
	int ncap = nvg__curveDivs(w, NVG_PI, ctx->tessTol);	// Calculate divisions per half circle.

	w += aa * 0.5f;

	// Disable the gradient used for antialiasing when antialiasing is not used.
	if (aa == 0.0f) {
		u0 = 0.5f;
		u1 = 0.5f;
	}

	nvg__calculateJoins(ctx, w, lineJoin, miterLimit);

	// Calculate max vertex usage.
	cverts = 0;
	for (i = 0; i < cache->npaths; i++) {
		NVGpath* path = &cache->paths[i];
		int loop = (path->closed == 0) ? 0 : 1;
		if (lineJoin == NVG_ROUND)
			cverts += (path->count + path->nbevel*(ncap+2) + 1) * 2; // plus one for loop
		else
			cverts += (path->count + path->nbevel*5 + 1) * 2; // plus one for loop
		if (loop == 0) {
			// space for caps
			if (lineCap == NVG_ROUND) {
				cverts += (ncap*2 + 2)*2;
			} else {
				cverts += (3+3)*2;
			}
		}
	}

	verts = nvg__allocTempVerts(ctx, cverts);
	if (verts == NULL) return 0;

	for (i = 0; i < cache->npaths; i++) {
		NVGpath* path = &cache->paths[i];
		NVGpoint* pts = &cache->points[path->first];
		NVGpoint* p0;
		NVGpoint* p1;
		int s, e, loop;
		float dx, dy;

		path->fill = 0;
		path->nfill = 0;

		// Calculate fringe or stroke
		loop = (path->closed == 0) ? 0 : 1;
		dst = verts;
		path->stroke = dst;

		if (loop) {
			// Looping
			p0 = &pts[path->count-1];
			p1 = &pts[0];
			s = 0;
			e = path->count;
		} else {
			// Add cap
			p0 = &pts[0];
			p1 = &pts[1];
			s = 1;
			e = path->count-1;
		}

		if (loop == 0) {
			// Add cap
			dx = p1->x - p0->x;
			dy = p1->y - p0->y;
			nvg__normalize(&dx, &dy);
			if (lineCap == NVG_BUTT)
				dst = nvg__buttCapStart(dst, p0, dx, dy, w, -aa*0.5f, aa, u0, u1);
			else if (lineCap == NVG_BUTT || lineCap == NVG_SQUARE)
				dst = nvg__buttCapStart(dst, p0, dx, dy, w, w-aa, aa, u0, u1);
			else if (lineCap == NVG_ROUND)
				dst = nvg__roundCapStart(dst, p0, dx, dy, w, ncap, aa, u0, u1);
		}

		for (j = s; j < e; ++j) {
			if ((p1->flags & (NVG_PT_BEVEL | NVG_PR_INNERBEVEL)) != 0) {
				if (lineJoin == NVG_ROUND) {
					dst = nvg__roundJoin(dst, p0, p1, w, w, u0, u1, ncap, aa);
				} else {
					dst = nvg__bevelJoin(dst, p0, p1, w, w, u0, u1, aa);
				}
			} else {
				nvg__vset(dst, p1->x + (p1->dmx * w), p1->y + (p1->dmy * w), u0,1); dst++;
				nvg__vset(dst, p1->x - (p1->dmx * w), p1->y - (p1->dmy * w), u1,1); dst++;
			}
			p0 = p1++;
		}

		if (loop) {
			// Loop it
			nvg__vset(dst, verts[0].x, verts[0].y, u0,1); dst++;
			nvg__vset(dst, verts[1].x, verts[1].y, u1,1); dst++;
		} else {
			// Add cap
			dx = p1->x - p0->x;
			dy = p1->y - p0->y;
			nvg__normalize(&dx, &dy);
			if (lineCap == NVG_BUTT)
				dst = nvg__buttCapEnd(dst, p1, dx, dy, w, -aa*0.5f, aa, u0, u1);
			else if (lineCap == NVG_BUTT || lineCap == NVG_SQUARE)
				dst = nvg__buttCapEnd(dst, p1, dx, dy, w, w-aa, aa, u0, u1);
			else if (lineCap == NVG_ROUND)
				dst = nvg__roundCapEnd(dst, p1, dx, dy, w, ncap, aa, u0, u1);
		}

		path->nstroke = (int)(dst - verts);

		verts = dst;
	}

	return 1;
}

static int nvg__expandFill(NVGcontext* ctx, float w, int lineJoin, float miterLimit)
{
	NVGpathCache* cache = ctx->cache;
	NVGvertex* verts;
	NVGvertex* dst;
	int cverts, convex, i, j;
	float aa = ctx->fringeWidth;
	int fringe = w > 0.0f;

	nvg__calculateJoins(ctx, w, lineJoin, miterLimit);

	// Calculate max vertex usage.
	cverts = 0;
	for (i = 0; i < cache->npaths; i++) {
		NVGpath* path = &cache->paths[i];
		cverts += path->count + path->nbevel + 1;
		if (fringe)
			cverts += (path->count + path->nbevel*5 + 1) * 2; // plus one for loop
	}

	verts = nvg__allocTempVerts(ctx, cverts);
	if (verts == NULL) return 0;

	convex = cache->npaths == 1 && cache->paths[0].convex;

	for (i = 0; i < cache->npaths; i++) {
		NVGpath* path = &cache->paths[i];
		NVGpoint* pts = &cache->points[path->first];
		NVGpoint* p0;
		NVGpoint* p1;
		float rw, lw, woff;
		float ru, lu;

		// Calculate shape vertices.
		woff = 0.5f*aa;
		dst = verts;
		path->fill = dst;

		if (fringe) {
			// Looping
			p0 = &pts[path->count-1];
			p1 = &pts[0];
			for (j = 0; j < path->count; ++j) {
				if (p1->flags & NVG_PT_BEVEL) {
					float dlx0 = p0->dy;
					float dly0 = -p0->dx;
					float dlx1 = p1->dy;
					float dly1 = -p1->dx;
					if (p1->flags & NVG_PT_LEFT) {
						float lx = p1->x + p1->dmx * woff;
						float ly = p1->y + p1->dmy * woff;
						nvg__vset(dst, lx, ly, 0.5f,1); dst++;
					} else {
						float lx0 = p1->x + dlx0 * woff;
						float ly0 = p1->y + dly0 * woff;
						float lx1 = p1->x + dlx1 * woff;
						float ly1 = p1->y + dly1 * woff;
						nvg__vset(dst, lx0, ly0, 0.5f,1); dst++;
						nvg__vset(dst, lx1, ly1, 0.5f,1); dst++;
					}
				} else {
					nvg__vset(dst, p1->x + (p1->dmx * woff), p1->y + (p1->dmy * woff), 0.5f,1); dst++;
				}
				p0 = p1++;
			}
		} else {
			for (j = 0; j < path->count; ++j) {
				nvg__vset(dst, pts[j].x, pts[j].y, 0.5f,1);
				dst++;
			}
		}

		path->nfill = (int)(dst - verts);
		verts = dst;

		// Calculate fringe
		if (fringe) {
			lw = w + woff;
			rw = w - woff;
			lu = 0;
			ru = 1;
			dst = verts;
			path->stroke = dst;

			// Create only half a fringe for convex shapes so that
			// the shape can be rendered without stenciling.
			if (convex) {
				lw = woff;	// This should generate the same vertex as fill inset above.
				lu = 0.5f;	// Set outline fade at middle.
			}

			// Looping
			p0 = &pts[path->count-1];
			p1 = &pts[0];

			for (j = 0; j < path->count; ++j) {
				if ((p1->flags & (NVG_PT_BEVEL | NVG_PR_INNERBEVEL)) != 0) {
					dst = nvg__bevelJoin(dst, p0, p1, lw, rw, lu, ru, ctx->fringeWidth);
				} else {
					nvg__vset(dst, p1->x + (p1->dmx * lw), p1->y + (p1->dmy * lw), lu,1); dst++;
					nvg__vset(dst, p1->x - (p1->dmx * rw), p1->y - (p1->dmy * rw), ru,1); dst++;
				}
				p0 = p1++;
			}

			// Loop it
			nvg__vset(dst, verts[0].x, verts[0].y, lu,1); dst++;
			nvg__vset(dst, verts[1].x, verts[1].y, ru,1); dst++;

			path->nstroke = (int)(dst - verts);
			verts = dst;
		} else {
			path->stroke = NULL;
			path->nstroke = 0;
		}
	}

	return 1;
}


// Draw
void nvgBeginPath(NVGcontext* ctx)
{
	ctx->ncommands = 0;
	nvg__clearPathCache(ctx);
}

void nvgMoveTo(NVGcontext* ctx, float x, float y)
{
	float vals[] = { NVG_MOVETO, x, y };
	nvg__appendCommands(ctx, vals, NVG_COUNTOF(vals));
}

void nvgLineTo(NVGcontext* ctx, float x, float y)
{
	float vals[] = { NVG_LINETO, x, y };
	nvg__appendCommands(ctx, vals, NVG_COUNTOF(vals));
}

void nvgBezierTo(NVGcontext* ctx, float c1x, float c1y, float c2x, float c2y, float x, float y)
{
	float vals[] = { NVG_BEZIERTO, c1x, c1y, c2x, c2y, x, y };
	nvg__appendCommands(ctx, vals, NVG_COUNTOF(vals));
}

void nvgQuadTo(NVGcontext* ctx, float cx, float cy, float x, float y)
{
    float x0 = ctx->commandx;
    float y0 = ctx->commandy;
    float vals[] = { NVG_BEZIERTO,
        x0 + 2.0f/3.0f*(cx - x0), y0 + 2.0f/3.0f*(cy - y0),
        x + 2.0f/3.0f*(cx - x), y + 2.0f/3.0f*(cy - y),
        x, y };
    nvg__appendCommands(ctx, vals, NVG_COUNTOF(vals));
}

void nvgArcTo(NVGcontext* ctx, float x1, float y1, float x2, float y2, float radius)
{
	float x0 = ctx->commandx;
	float y0 = ctx->commandy;
	float dx0,dy0, dx1,dy1, a, d, cx,cy, a0,a1;
	int dir;

	if (ctx->ncommands == 0) {
		return;
	}

	// Handle degenerate cases.
	if (nvg__ptEquals(x0,y0, x1,y1, ctx->distTol) ||
		nvg__ptEquals(x1,y1, x2,y2, ctx->distTol) ||
		nvg__distPtSeg(x1,y1, x0,y0, x2,y2) < ctx->distTol*ctx->distTol ||
		radius < ctx->distTol) {
		nvgLineTo(ctx, x1,y1);
		return;
	}

	// Calculate tangential circle to lines (x0,y0)-(x1,y1) and (x1,y1)-(x2,y2).
	dx0 = x0-x1;
	dy0 = y0-y1;
	dx1 = x2-x1;
	dy1 = y2-y1;
	nvg__normalize(&dx0,&dy0);
	nvg__normalize(&dx1,&dy1);
	a = nvg__acosf(dx0*dx1 + dy0*dy1);
	d = radius / nvg__tanf(a/2.0f);

//	printf("a=%f° d=%f\n", a/NVG_PI*180.0f, d);

	if (d > 10000.0f) {
		nvgLineTo(ctx, x1,y1);
		return;
	}

	if (nvg__cross(dx0,dy0, dx1,dy1) > 0.0f) {
		cx = x1 + dx0*d + dy0*radius;
		cy = y1 + dy0*d + -dx0*radius;
		a0 = nvg__atan2f(dx0, -dy0);
		a1 = nvg__atan2f(-dx1, dy1);
		dir = NVG_CW;
//		printf("CW c=(%f, %f) a0=%f° a1=%f°\n", cx, cy, a0/NVG_PI*180.0f, a1/NVG_PI*180.0f);
	} else {
		cx = x1 + dx0*d + -dy0*radius;
		cy = y1 + dy0*d + dx0*radius;
		a0 = nvg__atan2f(-dx0, dy0);
		a1 = nvg__atan2f(dx1, -dy1);
		dir = NVG_CCW;
//		printf("CCW c=(%f, %f) a0=%f° a1=%f°\n", cx, cy, a0/NVG_PI*180.0f, a1/NVG_PI*180.0f);
	}

	nvgArc(ctx, cx, cy, radius, a0, a1, dir);
}

void nvgClosePath(NVGcontext* ctx)
{
	float vals[] = { NVG_CLOSE };
	nvg__appendCommands(ctx, vals, NVG_COUNTOF(vals));
}

void nvgPathWinding(NVGcontext* ctx, int dir)
{
	float vals[] = { NVG_WINDING, (float)dir };
	nvg__appendCommands(ctx, vals, NVG_COUNTOF(vals));
}

void nvgArc(NVGcontext* ctx, float cx, float cy, float r, float a0, float a1, int dir)
{
	float a = 0, da = 0, hda = 0, kappa = 0;
	float dx = 0, dy = 0, x = 0, y = 0, tanx = 0, tany = 0;
	float px = 0, py = 0, ptanx = 0, ptany = 0;
	float vals[3 + 5*7 + 100];
	int i, ndivs, nvals;
	int move = ctx->ncommands > 0 ? NVG_LINETO : NVG_MOVETO;

	// Clamp angles
	da = a1 - a0;
	if (dir == NVG_CW) {
		if (nvg__absf(da) >= NVG_PI*2) {
			da = NVG_PI*2;
		} else {
			while (da < 0.0f) da += NVG_PI*2;
		}
	} else {
		if (nvg__absf(da) >= NVG_PI*2) {
			da = -NVG_PI*2;
		} else {
			while (da > 0.0f) da -= NVG_PI*2;
		}
	}

	// Split arc into max 90 degree segments.
	ndivs = nvg__maxi(1, nvg__mini((int)(nvg__absf(da) / (NVG_PI*0.5f) + 0.5f), 5));
	hda = (da / (float)ndivs) / 2.0f;
	kappa = nvg__absf(4.0f / 3.0f * (1.0f - nvg__cosf(hda)) / nvg__sinf(hda));

	if (dir == NVG_CCW)
		kappa = -kappa;

	nvals = 0;
	for (i = 0; i <= ndivs; i++) {
		a = a0 + da * (i/(float)ndivs);
		dx = nvg__cosf(a);
		dy = nvg__sinf(a);
		x = cx + dx*r;
		y = cy + dy*r;
		tanx = -dy*r*kappa;
		tany = dx*r*kappa;

		if (i == 0) {
			vals[nvals++] = (float)move;
			vals[nvals++] = x;
			vals[nvals++] = y;
		} else {
			vals[nvals++] = NVG_BEZIERTO;
			vals[nvals++] = px+ptanx;
			vals[nvals++] = py+ptany;
			vals[nvals++] = x-tanx;
			vals[nvals++] = y-tany;
			vals[nvals++] = x;
			vals[nvals++] = y;
		}
		px = x;
		py = y;
		ptanx = tanx;
		ptany = tany;
	}

	nvg__appendCommands(ctx, vals, nvals);
}

void nvgRect(NVGcontext* ctx, float x, float y, float w, float h)
{
	float vals[] = {
		NVG_MOVETO, x,y,
		NVG_LINETO, x,y+h,
		NVG_LINETO, x+w,y+h,
		NVG_LINETO, x+w,y,
		NVG_CLOSE
	};
	nvg__appendCommands(ctx, vals, NVG_COUNTOF(vals));
}

void nvgRoundedRect(NVGcontext* ctx, float x, float y, float w, float h, float r)
{
	nvgRoundedRectVarying(ctx, x, y, w, h, r, r, r, r);
}

void nvgRoundedRectVarying(NVGcontext* ctx, float x, float y, float w, float h, float radTopLeft, float radTopRight, float radBottomRight, float radBottomLeft)
{
	if(radTopLeft < 0.1f && radTopRight < 0.1f && radBottomRight < 0.1f && radBottomLeft < 0.1f) {
		nvgRect(ctx, x, y, w, h);
		return;
	} else {
		float halfw = nvg__absf(w)*0.5f;
		float halfh = nvg__absf(h)*0.5f;
		float rxBL = nvg__minf(radBottomLeft, halfw) * nvg__signf(w), ryBL = nvg__minf(radBottomLeft, halfh) * nvg__signf(h);
		float rxBR = nvg__minf(radBottomRight, halfw) * nvg__signf(w), ryBR = nvg__minf(radBottomRight, halfh) * nvg__signf(h);
		float rxTR = nvg__minf(radTopRight, halfw) * nvg__signf(w), ryTR = nvg__minf(radTopRight, halfh) * nvg__signf(h);
		float rxTL = nvg__minf(radTopLeft, halfw) * nvg__signf(w), ryTL = nvg__minf(radTopLeft, halfh) * nvg__signf(h);
		float vals[] = {
			NVG_MOVETO, x, y + ryTL,
			NVG_LINETO, x, y + h - ryBL,
			NVG_BEZIERTO, x, y + h - ryBL*(1 - NVG_KAPPA90), x + rxBL*(1 - NVG_KAPPA90), y + h, x + rxBL, y + h,
			NVG_LINETO, x + w - rxBR, y + h,
			NVG_BEZIERTO, x + w - rxBR*(1 - NVG_KAPPA90), y + h, x + w, y + h - ryBR*(1 - NVG_KAPPA90), x + w, y + h - ryBR,
			NVG_LINETO, x + w, y + ryTR,
			NVG_BEZIERTO, x + w, y + ryTR*(1 - NVG_KAPPA90), x + w - rxTR*(1 - NVG_KAPPA90), y, x + w - rxTR, y,
			NVG_LINETO, x + rxTL, y,
			NVG_BEZIERTO, x + rxTL*(1 - NVG_KAPPA90), y, x, y + ryTL*(1 - NVG_KAPPA90), x, y + ryTL,
			NVG_CLOSE
		};
		nvg__appendCommands(ctx, vals, NVG_COUNTOF(vals));
	}
}

void nvgEllipse(NVGcontext* ctx, float cx, float cy, float rx, float ry)
{
	float vals[] = {
		NVG_MOVETO, cx-rx, cy,
		NVG_BEZIERTO, cx-rx, cy+ry*NVG_KAPPA90, cx-rx*NVG_KAPPA90, cy+ry, cx, cy+ry,
		NVG_BEZIERTO, cx+rx*NVG_KAPPA90, cy+ry, cx+rx, cy+ry*NVG_KAPPA90, cx+rx, cy,
		NVG_BEZIERTO, cx+rx, cy-ry*NVG_KAPPA90, cx+rx*NVG_KAPPA90, cy-ry, cx, cy-ry,
		NVG_BEZIERTO, cx-rx*NVG_KAPPA90, cy-ry, cx-rx, cy-ry*NVG_KAPPA90, cx-rx, cy,
		NVG_CLOSE
	};
	nvg__appendCommands(ctx, vals, NVG_COUNTOF(vals));
}

void nvgCircle(NVGcontext* ctx, float cx, float cy, float r)
{
	nvgEllipse(ctx, cx,cy, r,r);
}

void nvgDebugDumpPathCache(NVGcontext* ctx)
{
	const NVGpath* path;
	int i, j;

	printf("Dumping %d cached paths\n", ctx->cache->npaths);
	for (i = 0; i < ctx->cache->npaths; i++) {
		path = &ctx->cache->paths[i];
		printf(" - Path %d\n", i);
		if (path->nfill) {
			printf("   - fill: %d\n", path->nfill);
			for (j = 0; j < path->nfill; j++)
				printf("%f\t%f\n", path->fill[j].x, path->fill[j].y);
		}
		if (path->nstroke) {
			printf("   - stroke: %d\n", path->nstroke);
			for (j = 0; j < path->nstroke; j++)
				printf("%f\t%f\n", path->stroke[j].x, path->stroke[j].y);
		}
	}
}

void nvgFill(NVGcontext* ctx)
{
	NVGstate* state = nvg__getState(ctx);
	const NVGpath* path;
	NVGpaint fillPaint = state->fill;
	int i;

	nvg__flattenPaths(ctx);
	if (ctx->params.edgeAntiAlias && state->shapeAntiAlias)
		nvg__expandFill(ctx, ctx->fringeWidth, NVG_MITER, 2.4f);
	else
		nvg__expandFill(ctx, 0.0f, NVG_MITER, 2.4f);

	// Apply global alpha
	fillPaint.innerColor.a *= state->alpha;
	fillPaint.outerColor.a *= state->alpha;

	ctx->params.renderFill(ctx->params.userPtr, &fillPaint, state->compositeOperation, &state->scissor, ctx->fringeWidth,
						   ctx->cache->bounds, ctx->cache->paths, ctx->cache->npaths);

	// Count triangles
	for (i = 0; i < ctx->cache->npaths; i++) {
		path = &ctx->cache->paths[i];
		ctx->fillTriCount += path->nfill-2;
		ctx->fillTriCount += path->nstroke-2;
		ctx->drawCallCount += 2;
	}
}

void nvgStroke(NVGcontext* ctx)
{
	NVGstate* state = nvg__getState(ctx);
	float scale = nvg__getAverageScale(state->xform);
	float strokeWidth = nvg__clampf(state->strokeWidth * scale, 0.0f, 200.0f);
	NVGpaint strokePaint = state->stroke;
	const NVGpath* path;
	int i;


	if (strokeWidth < ctx->fringeWidth) {
		// If the stroke width is less than pixel size, use alpha to emulate coverage.
		// Since coverage is area, scale by alpha*alpha.
		float alpha = nvg__clampf(strokeWidth / ctx->fringeWidth, 0.0f, 1.0f);
		strokePaint.innerColor.a *= alpha*alpha;
		strokePaint.outerColor.a *= alpha*alpha;
		strokeWidth = ctx->fringeWidth;
	}

	// Apply global alpha
	strokePaint.innerColor.a *= state->alpha;
	strokePaint.outerColor.a *= state->alpha;

	nvg__flattenPaths(ctx);

	if (ctx->params.edgeAntiAlias && state->shapeAntiAlias)
		nvg__expandStroke(ctx, strokeWidth*0.5f, ctx->fringeWidth, state->lineCap, state->lineJoin, state->miterLimit);
	else
		nvg__expandStroke(ctx, strokeWidth*0.5f, 0.0f, state->lineCap, state->lineJoin, state->miterLimit);

	ctx->params.renderStroke(ctx->params.userPtr, &strokePaint, state->compositeOperation, &state->scissor, ctx->fringeWidth,
							 strokeWidth, ctx->cache->paths, ctx->cache->npaths);

	// Count triangles
	for (i = 0; i < ctx->cache->npaths; i++) {
		path = &ctx->cache->paths[i];
		ctx->strokeTriCount += path->nstroke-2;
		ctx->drawCallCount++;
	}
}

// Add fonts
int nvgCreateFont(NVGcontext* ctx, const char* name, const char* path)
{
	return fonsAddFont(ctx->fs, name, path);
}

int nvgCreateFontMem(NVGcontext* ctx, const char* name, unsigned char* data, int ndata, int freeData)
{
	return fonsAddFontMem(ctx->fs, name, data, ndata, freeData);
}

int nvgFindFont(NVGcontext* ctx, const char* name)
{
	if (name == NULL) return -1;
	return fonsGetFontByName(ctx->fs, name);
}


int nvgAddFallbackFontId(NVGcontext* ctx, int baseFont, int fallbackFont)
{
	if(baseFont == -1 || fallbackFont == -1) return 0;
	return fonsAddFallbackFont(ctx->fs, baseFont, fallbackFont);
}

int nvgAddFallbackFont(NVGcontext* ctx, const char* baseFont, const char* fallbackFont)
{
	return nvgAddFallbackFontId(ctx, nvgFindFont(ctx, baseFont), nvgFindFont(ctx, fallbackFont));
}

// State setting
void nvgFontSize(NVGcontext* ctx, float size)
{
	NVGstate* state = nvg__getState(ctx);
	state->fontSize = size;
}

void nvgFontBlur(NVGcontext* ctx, float blur)
{
	NVGstate* state = nvg__getState(ctx);
	state->fontBlur = blur;
}

void nvgTextLetterSpacing(NVGcontext* ctx, float spacing)
{
	NVGstate* state = nvg__getState(ctx);
	state->letterSpacing = spacing;
}

void nvgTextLineHeight(NVGcontext* ctx, float lineHeight)
{
	NVGstate* state = nvg__getState(ctx);
	state->lineHeight = lineHeight;
}

void nvgTextAlign(NVGcontext* ctx, int align)
{
	NVGstate* state = nvg__getState(ctx);
	state->textAlign = align;
}

void nvgFontFaceId(NVGcontext* ctx, int font)
{
	NVGstate* state = nvg__getState(ctx);
	state->fontId = font;
}

void nvgFontFace(NVGcontext* ctx, const char* font)
{
	NVGstate* state = nvg__getState(ctx);
	state->fontId = fonsGetFontByName(ctx->fs, font);
}

static float nvg__quantize(float a, float d)
{
	return ((int)(a / d + 0.5f)) * d;
}

static float nvg__getFontScale(NVGstate* state)
{
	return nvg__minf(nvg__quantize(nvg__getAverageScale(state->xform), 0.01f), 4.0f);
}

static void nvg__flushTextTexture(NVGcontext* ctx)
{
	int dirty[4];

	if (fonsValidateTexture(ctx->fs, dirty)) {
		int fontImage = ctx->fontImages[ctx->fontImageIdx];
		// Update texture
		if (fontImage != 0) {
			int iw, ih;
			const unsigned char* data = fonsGetTextureData(ctx->fs, &iw, &ih);
			int x = dirty[0];
			int y = dirty[1];
			int w = dirty[2] - dirty[0];
			int h = dirty[3] - dirty[1];
			ctx->params.renderUpdateTexture(ctx->params.userPtr, fontImage, x,y, w,h, data);
		}
	}
}

static int nvg__allocTextAtlas(NVGcontext* ctx)
{
	int iw, ih;
	nvg__flushTextTexture(ctx);
	if (ctx->fontImageIdx >= NVG_MAX_FONTIMAGES-1)
		return 0;
	// if next fontImage already have a texture
	if (ctx->fontImages[ctx->fontImageIdx+1] != 0)
		nvgImageSize(ctx, ctx->fontImages[ctx->fontImageIdx+1], &iw, &ih);
	else { // calculate the new font image size and create it.
		nvgImageSize(ctx, ctx->fontImages[ctx->fontImageIdx], &iw, &ih);
		if (iw > ih)
			ih *= 2;
		else
			iw *= 2;
		if (iw > NVG_MAX_FONTIMAGE_SIZE || ih > NVG_MAX_FONTIMAGE_SIZE)
			iw = ih = NVG_MAX_FONTIMAGE_SIZE;
		ctx->fontImages[ctx->fontImageIdx+1] = ctx->params.renderCreateTexture(ctx->params.userPtr, NVG_TEXTURE_ALPHA, iw, ih, 0, NULL);
	}
	++ctx->fontImageIdx;
	fonsResetAtlas(ctx->fs, iw, ih);
	return 1;
}

static void nvg__renderText(NVGcontext* ctx, NVGvertex* verts, int nverts)
{
	NVGstate* state = nvg__getState(ctx);
	NVGpaint paint = state->fill;

	// Render triangles.
	paint.image = ctx->fontImages[ctx->fontImageIdx];

	// Apply global alpha
	paint.innerColor.a *= state->alpha;
	paint.outerColor.a *= state->alpha;

	ctx->params.renderTriangles(ctx->params.userPtr, &paint, state->compositeOperation, &state->scissor, verts, nverts);

	ctx->drawCallCount++;
	ctx->textTriCount += nverts/3;
}

float nvgText(NVGcontext* ctx, float x, float y, const char* string, const char* end)
{
	NVGstate* state = nvg__getState(ctx);
	FONStextIter iter, prevIter;
	FONSquad q;
	NVGvertex* verts;
	float scale = nvg__getFontScale(state) * ctx->devicePxRatio;
	float invscale = 1.0f / scale;
	int cverts = 0;
	int nverts = 0;

	if (end == NULL)
		end = string + strlen(string);

	if (state->fontId == FONS_INVALID) return x;

	fonsSetSize(ctx->fs, state->fontSize*scale);
	fonsSetSpacing(ctx->fs, state->letterSpacing*scale);
	fonsSetBlur(ctx->fs, state->fontBlur*scale);
	fonsSetAlign(ctx->fs, state->textAlign);
	fonsSetFont(ctx->fs, state->fontId);

	cverts = nvg__maxi(2, (int)(end - string)) * 6; // conservative estimate.
	verts = nvg__allocTempVerts(ctx, cverts);
	if (verts == NULL) return x;

	fonsTextIterInit(ctx->fs, &iter, x*scale, y*scale, string, end, FONS_GLYPH_BITMAP_REQUIRED);
	prevIter = iter;
	while (fonsTextIterNext(ctx->fs, &iter, &q)) {
		float c[4*2];
		if (iter.prevGlyphIndex == -1) { // can not retrieve glyph?
			if (nverts != 0) {
				nvg__renderText(ctx, verts, nverts);
				nverts = 0;
			}
			if (!nvg__allocTextAtlas(ctx))
				break; // no memory :(
			iter = prevIter;
			fonsTextIterNext(ctx->fs, &iter, &q); // try again
			if (iter.prevGlyphIndex == -1) // still can not find glyph?
				break;
		}
		prevIter = iter;
		// Transform corners.
		nvgTransformPoint(&c[0],&c[1], state->xform, q.x0*invscale, q.y0*invscale);
		nvgTransformPoint(&c[2],&c[3], state->xform, q.x1*invscale, q.y0*invscale);
		nvgTransformPoint(&c[4],&c[5], state->xform, q.x1*invscale, q.y1*invscale);
		nvgTransformPoint(&c[6],&c[7], state->xform, q.x0*invscale, q.y1*invscale);
		// Create triangles
		if (nverts+6 <= cverts) {
			nvg__vset(&verts[nverts], c[0], c[1], q.s0, q.t0); nverts++;
			nvg__vset(&verts[nverts], c[4], c[5], q.s1, q.t1); nverts++;
			nvg__vset(&verts[nverts], c[2], c[3], q.s1, q.t0); nverts++;
			nvg__vset(&verts[nverts], c[0], c[1], q.s0, q.t0); nverts++;
			nvg__vset(&verts[nverts], c[6], c[7], q.s0, q.t1); nverts++;
			nvg__vset(&verts[nverts], c[4], c[5], q.s1, q.t1); nverts++;
		}
	}

	// TODO: add back-end bit to do this just once per frame.
	nvg__flushTextTexture(ctx);

	nvg__renderText(ctx, verts, nverts);

	return iter.nextx / scale;
}

void nvgTextBox(NVGcontext* ctx, float x, float y, float breakRowWidth, const char* string, const char* end)
{
	NVGstate* state = nvg__getState(ctx);
	NVGtextRow rows[2];
	int nrows = 0, i;
	int oldAlign = state->textAlign;
	int haling = state->textAlign & (NVG_ALIGN_LEFT | NVG_ALIGN_CENTER | NVG_ALIGN_RIGHT);
	int valign = state->textAlign & (NVG_ALIGN_TOP | NVG_ALIGN_MIDDLE | NVG_ALIGN_BOTTOM | NVG_ALIGN_BASELINE);
	float lineh = 0;

	if (state->fontId == FONS_INVALID) return;

	nvgTextMetrics(ctx, NULL, NULL, &lineh);

	state->textAlign = NVG_ALIGN_LEFT | valign;

	while ((nrows = nvgTextBreakLines(ctx, string, end, breakRowWidth, rows, 2))) {
		for (i = 0; i < nrows; i++) {
			NVGtextRow* row = &rows[i];
			if (haling & NVG_ALIGN_LEFT)
				nvgText(ctx, x, y, row->start, row->end);
			else if (haling & NVG_ALIGN_CENTER)
				nvgText(ctx, x + breakRowWidth*0.5f - row->width*0.5f, y, row->start, row->end);
			else if (haling & NVG_ALIGN_RIGHT)
				nvgText(ctx, x + breakRowWidth - row->width, y, row->start, row->end);
			y += lineh * state->lineHeight;
		}
		string = rows[nrows-1].next;
	}

	state->textAlign = oldAlign;
}

int nvgTextGlyphPositions(NVGcontext* ctx, float x, float y, const char* string, const char* end, NVGglyphPosition* positions, int maxPositions)
{
	NVGstate* state = nvg__getState(ctx);
	float scale = nvg__getFontScale(state) * ctx->devicePxRatio;
	float invscale = 1.0f / scale;
	FONStextIter iter, prevIter;
	FONSquad q;
	int npos = 0;

	if (state->fontId == FONS_INVALID) return 0;

	if (end == NULL)
		end = string + strlen(string);

	if (string == end)
		return 0;

	fonsSetSize(ctx->fs, state->fontSize*scale);
	fonsSetSpacing(ctx->fs, state->letterSpacing*scale);
	fonsSetBlur(ctx->fs, state->fontBlur*scale);
	fonsSetAlign(ctx->fs, state->textAlign);
	fonsSetFont(ctx->fs, state->fontId);

	fonsTextIterInit(ctx->fs, &iter, x*scale, y*scale, string, end, FONS_GLYPH_BITMAP_OPTIONAL);
	prevIter = iter;
	while (fonsTextIterNext(ctx->fs, &iter, &q)) {
		if (iter.prevGlyphIndex < 0 && nvg__allocTextAtlas(ctx)) { // can not retrieve glyph?
			iter = prevIter;
			fonsTextIterNext(ctx->fs, &iter, &q); // try again
		}
		prevIter = iter;
		positions[npos].str = iter.str;
		positions[npos].x = iter.x * invscale;
		positions[npos].minx = nvg__minf(iter.x, q.x0) * invscale;
		positions[npos].maxx = nvg__maxf(iter.nextx, q.x1) * invscale;
		npos++;
		if (npos >= maxPositions)
			break;
	}

	return npos;
}

enum NVGcodepointType {
	NVG_SPACE,
	NVG_NEWLINE,
	NVG_CHAR,
	NVG_CJK_CHAR,
};

int nvgTextBreakLines(NVGcontext* ctx, const char* string, const char* end, float breakRowWidth, NVGtextRow* rows, int maxRows)
{
	NVGstate* state = nvg__getState(ctx);
	float scale = nvg__getFontScale(state) * ctx->devicePxRatio;
	float invscale = 1.0f / scale;
	FONStextIter iter, prevIter;
	FONSquad q;
	int nrows = 0;
	float rowStartX = 0;
	float rowWidth = 0;
	float rowMinX = 0;
	float rowMaxX = 0;
	const char* rowStart = NULL;
	const char* rowEnd = NULL;
	const char* wordStart = NULL;
	float wordStartX = 0;
	float wordMinX = 0;
	const char* breakEnd = NULL;
	float breakWidth = 0;
	float breakMaxX = 0;
	int type = NVG_SPACE, ptype = NVG_SPACE;
	unsigned int pcodepoint = 0;

	if (maxRows == 0) return 0;
	if (state->fontId == FONS_INVALID) return 0;

	if (end == NULL)
		end = string + strlen(string);

	if (string == end) return 0;

	fonsSetSize(ctx->fs, state->fontSize*scale);
	fonsSetSpacing(ctx->fs, state->letterSpacing*scale);
	fonsSetBlur(ctx->fs, state->fontBlur*scale);
	fonsSetAlign(ctx->fs, state->textAlign);
	fonsSetFont(ctx->fs, state->fontId);

	breakRowWidth *= scale;

	fonsTextIterInit(ctx->fs, &iter, 0, 0, string, end, FONS_GLYPH_BITMAP_OPTIONAL);
	prevIter = iter;
	while (fonsTextIterNext(ctx->fs, &iter, &q)) {
		if (iter.prevGlyphIndex < 0 && nvg__allocTextAtlas(ctx)) { // can not retrieve glyph?
			iter = prevIter;
			fonsTextIterNext(ctx->fs, &iter, &q); // try again
		}
		prevIter = iter;
		switch (iter.codepoint) {
			case 9:			// \t
			case 11:		// \v
			case 12:		// \f
			case 32:		// space
			case 0x00a0:	// NBSP
				type = NVG_SPACE;
				break;
			case 10:		// \n
				type = pcodepoint == 13 ? NVG_SPACE : NVG_NEWLINE;
				break;
			case 13:		// \r
				type = pcodepoint == 10 ? NVG_SPACE : NVG_NEWLINE;
				break;
			case 0x0085:	// NEL
				type = NVG_NEWLINE;
				break;
			default:
				if ((iter.codepoint >= 0x4E00 && iter.codepoint <= 0x9FFF) ||
					(iter.codepoint >= 0x3000 && iter.codepoint <= 0x30FF) ||
					(iter.codepoint >= 0xFF00 && iter.codepoint <= 0xFFEF) ||
					(iter.codepoint >= 0x1100 && iter.codepoint <= 0x11FF) ||
					(iter.codepoint >= 0x3130 && iter.codepoint <= 0x318F) ||
					(iter.codepoint >= 0xAC00 && iter.codepoint <= 0xD7AF))
					type = NVG_CJK_CHAR;
				else
					type = NVG_CHAR;
				break;
		}

		if (type == NVG_NEWLINE) {
			// Always handle new lines.
			rows[nrows].start = rowStart != NULL ? rowStart : iter.str;
			rows[nrows].end = rowEnd != NULL ? rowEnd : iter.str;
			rows[nrows].width = rowWidth * invscale;
			rows[nrows].minx = rowMinX * invscale;
			rows[nrows].maxx = rowMaxX * invscale;
			rows[nrows].next = iter.next;
			nrows++;
			if (nrows >= maxRows)
				return nrows;
			// Set null break point
			breakEnd = rowStart;
			breakWidth = 0.0;
			breakMaxX = 0.0;
			// Indicate to skip the white space at the beginning of the row.
			rowStart = NULL;
			rowEnd = NULL;
			rowWidth = 0;
			rowMinX = rowMaxX = 0;
		} else {
			if (rowStart == NULL) {
				// Skip white space until the beginning of the line
				if (type == NVG_CHAR || type == NVG_CJK_CHAR) {
					// The current char is the row so far
					rowStartX = iter.x;
					rowStart = iter.str;
					rowEnd = iter.next;
					rowWidth = iter.nextx - rowStartX; // q.x1 - rowStartX;
					rowMinX = q.x0 - rowStartX;
					rowMaxX = q.x1 - rowStartX;
					wordStart = iter.str;
					wordStartX = iter.x;
					wordMinX = q.x0 - rowStartX;
					// Set null break point
					breakEnd = rowStart;
					breakWidth = 0.0;
					breakMaxX = 0.0;
				}
			} else {
				float nextWidth = iter.nextx - rowStartX;

				// track last non-white space character
				if (type == NVG_CHAR || type == NVG_CJK_CHAR) {
					rowEnd = iter.next;
					rowWidth = iter.nextx - rowStartX;
					rowMaxX = q.x1 - rowStartX;
				}
				// track last end of a word
				if (((ptype == NVG_CHAR || ptype == NVG_CJK_CHAR) && type == NVG_SPACE) || type == NVG_CJK_CHAR) {
					breakEnd = iter.str;
					breakWidth = rowWidth;
					breakMaxX = rowMaxX;
				}
				// track last beginning of a word
				if ((ptype == NVG_SPACE && (type == NVG_CHAR || type == NVG_CJK_CHAR)) || type == NVG_CJK_CHAR) {
					wordStart = iter.str;
					wordStartX = iter.x;
					wordMinX = q.x0 - rowStartX;
				}

				// Break to new line when a character is beyond break width.
				if ((type == NVG_CHAR || type == NVG_CJK_CHAR) && nextWidth > breakRowWidth) {
					// The run length is too long, need to break to new line.
					if (breakEnd == rowStart) {
						// The current word is longer than the row length, just break it from here.
						rows[nrows].start = rowStart;
						rows[nrows].end = iter.str;
						rows[nrows].width = rowWidth * invscale;
						rows[nrows].minx = rowMinX * invscale;
						rows[nrows].maxx = rowMaxX * invscale;
						rows[nrows].next = iter.str;
						nrows++;
						if (nrows >= maxRows)
							return nrows;
						rowStartX = iter.x;
						rowStart = iter.str;
						rowEnd = iter.next;
						rowWidth = iter.nextx - rowStartX;
						rowMinX = q.x0 - rowStartX;
						rowMaxX = q.x1 - rowStartX;
						wordStart = iter.str;
						wordStartX = iter.x;
						wordMinX = q.x0 - rowStartX;
					} else {
						// Break the line from the end of the last word, and start new line from the beginning of the new.
						rows[nrows].start = rowStart;
						rows[nrows].end = breakEnd;
						rows[nrows].width = breakWidth * invscale;
						rows[nrows].minx = rowMinX * invscale;
						rows[nrows].maxx = breakMaxX * invscale;
						rows[nrows].next = wordStart;
						nrows++;
						if (nrows >= maxRows)
							return nrows;
						rowStartX = wordStartX;
						rowStart = wordStart;
						rowEnd = iter.next;
						rowWidth = iter.nextx - rowStartX;
						rowMinX = wordMinX;
						rowMaxX = q.x1 - rowStartX;
						// No change to the word start
					}
					// Set null break point
					breakEnd = rowStart;
					breakWidth = 0.0;
					breakMaxX = 0.0;
				}
			}
		}

		pcodepoint = iter.codepoint;
		ptype = type;
	}

	// Break the line from the end of the last word, and start new line from the beginning of the new.
	if (rowStart != NULL) {
		rows[nrows].start = rowStart;
		rows[nrows].end = rowEnd;
		rows[nrows].width = rowWidth * invscale;
		rows[nrows].minx = rowMinX * invscale;
		rows[nrows].maxx = rowMaxX * invscale;
		rows[nrows].next = end;
		nrows++;
	}

	return nrows;
}

float nvgTextBounds(NVGcontext* ctx, float x, float y, const char* string, const char* end, float* bounds)
{
	NVGstate* state = nvg__getState(ctx);
	float scale = nvg__getFontScale(state) * ctx->devicePxRatio;
	float invscale = 1.0f / scale;
	float width;

	if (state->fontId == FONS_INVALID) return 0;

	fonsSetSize(ctx->fs, state->fontSize*scale);
	fonsSetSpacing(ctx->fs, state->letterSpacing*scale);
	fonsSetBlur(ctx->fs, state->fontBlur*scale);
	fonsSetAlign(ctx->fs, state->textAlign);
	fonsSetFont(ctx->fs, state->fontId);

	width = fonsTextBounds(ctx->fs, x*scale, y*scale, string, end, bounds);
	if (bounds != NULL) {
		// Use line bounds for height.
		fonsLineBounds(ctx->fs, y*scale, &bounds[1], &bounds[3]);
		bounds[0] *= invscale;
		bounds[1] *= invscale;
		bounds[2] *= invscale;
		bounds[3] *= invscale;
	}
	return width * invscale;
}

void nvgTextBoxBounds(NVGcontext* ctx, float x, float y, float breakRowWidth, const char* string, const char* end, float* bounds)
{
	NVGstate* state = nvg__getState(ctx);
	NVGtextRow rows[2];
	float scale = nvg__getFontScale(state) * ctx->devicePxRatio;
	float invscale = 1.0f / scale;
	int nrows = 0, i;
	int oldAlign = state->textAlign;
	int haling = state->textAlign & (NVG_ALIGN_LEFT | NVG_ALIGN_CENTER | NVG_ALIGN_RIGHT);
	int valign = state->textAlign & (NVG_ALIGN_TOP | NVG_ALIGN_MIDDLE | NVG_ALIGN_BOTTOM | NVG_ALIGN_BASELINE);
	float lineh = 0, rminy = 0, rmaxy = 0;
	float minx, miny, maxx, maxy;

	if (state->fontId == FONS_INVALID) {
		if (bounds != NULL)
			bounds[0] = bounds[1] = bounds[2] = bounds[3] = 0.0f;
		return;
	}

	nvgTextMetrics(ctx, NULL, NULL, &lineh);

	state->textAlign = NVG_ALIGN_LEFT | valign;

	minx = maxx = x;
	miny = maxy = y;

	fonsSetSize(ctx->fs, state->fontSize*scale);
	fonsSetSpacing(ctx->fs, state->letterSpacing*scale);
	fonsSetBlur(ctx->fs, state->fontBlur*scale);
	fonsSetAlign(ctx->fs, state->textAlign);
	fonsSetFont(ctx->fs, state->fontId);
	fonsLineBounds(ctx->fs, 0, &rminy, &rmaxy);
	rminy *= invscale;
	rmaxy *= invscale;

	while ((nrows = nvgTextBreakLines(ctx, string, end, breakRowWidth, rows, 2))) {
		for (i = 0; i < nrows; i++) {
			NVGtextRow* row = &rows[i];
			float rminx, rmaxx, dx = 0;
			// Horizontal bounds
			if (haling & NVG_ALIGN_LEFT)
				dx = 0;
			else if (haling & NVG_ALIGN_CENTER)
				dx = breakRowWidth*0.5f - row->width*0.5f;
			else if (haling & NVG_ALIGN_RIGHT)
				dx = breakRowWidth - row->width;
			rminx = x + row->minx + dx;
			rmaxx = x + row->maxx + dx;
			minx = nvg__minf(minx, rminx);
			maxx = nvg__maxf(maxx, rmaxx);
			// Vertical bounds.
			miny = nvg__minf(miny, y + rminy);
			maxy = nvg__maxf(maxy, y + rmaxy);

			y += lineh * state->lineHeight;
		}
		string = rows[nrows-1].next;
	}

	state->textAlign = oldAlign;

	if (bounds != NULL) {
		bounds[0] = minx;
		bounds[1] = miny;
		bounds[2] = maxx;
		bounds[3] = maxy;
	}
}

void nvgTextMetrics(NVGcontext* ctx, float* ascender, float* descender, float* lineh)
{
	NVGstate* state = nvg__getState(ctx);
	float scale = nvg__getFontScale(state) * ctx->devicePxRatio;
	float invscale = 1.0f / scale;

	if (state->fontId == FONS_INVALID) return;

	fonsSetSize(ctx->fs, state->fontSize*scale);
	fonsSetSpacing(ctx->fs, state->letterSpacing*scale);
	fonsSetBlur(ctx->fs, state->fontBlur*scale);
	fonsSetAlign(ctx->fs, state->textAlign);
	fonsSetFont(ctx->fs, state->fontId);

	fonsVertMetrics(ctx->fs, ascender, descender, lineh);
	if (ascender != NULL)
		*ascender *= invscale;
	if (descender != NULL)
		*descender *= invscale;
	if (lineh != NULL)
		*lineh *= invscale;
}
// vim: ft=c nu noet ts=4


// nanovg hit test
#define NVG_PICK_EPS	0.0001f

// Segment flags
enum NVGsegmentFlags {
	NVG_PICK_CORNER	= 1,
	NVG_PICK_BEVEL = 2,
	NVG_PICK_INNERBEVEL	= 4,
	NVG_PICK_CAP = 8,
	NVG_PICK_ENDCAP = 16,
};

// Path flags
enum NVGpathFlags {
	NVG_PICK_SCISSOR = 1,
	NVG_PICK_STROKE = 2,
	NVG_PICK_FILL = 4,
};

//#define NVG_PICK_DEBUG
#ifdef NVG_PICK_DEBUG
	int g_ndebugBounds = 0;
	float g_debugBounds[256][4];
	int g_ndebugLines = 0;
	float g_debugLines[256][4];

	#define NVG_PICK_DEBUG_NEWFRAME() \
		{ \
			g_ndebugBounds = 0; \
			g_ndebugLines = 0; \
		}

	#define NVG_PICK_DEBUG_BOUNDS(bounds) memcpy(&g_debugBounds[g_ndebugBounds++][0], bounds, sizeof(float) * 4)
	#define NVG_PICK_DEBUG_LINE(A, B) \
		{	memcpy(&g_debugLines[g_ndebugLines][0], (A), sizeof(float) * 2); \
			memcpy(&g_debugLines[g_ndebugLines++][2], (B), sizeof(float) * 2); }
	#define NVG_PICK_DEBUG_VECTOR(A, D) \
		{	memcpy(&g_debugLines[g_ndebugLines][0], (A), sizeof(float) * 2); \
			g_debugLines[g_ndebugLines][2] = (A)[0] + (D)[0]; \
			g_debugLines[g_ndebugLines][3] = (A)[1] + (D)[1]; \
			++g_ndebugLines; \
		}
	#define NVG_PICK_DEBUG_VECTOR_SCALE(A, D, S) \
		{	memcpy(&g_debugLines[g_ndebugLines][0], (A), sizeof(float) * 2); \
			g_debugLines[g_ndebugLines][2] = (A)[0] + (D)[0] * (S); \
			g_debugLines[g_ndebugLines][3] = (A)[1] + (D)[1] * (S); \
			++g_ndebugLines; \
		}
#else
	#define NVG_PICK_DEBUG_NEWFRAME()
	#define NVG_PICK_DEBUG_BOUNDS(bounds)
	#define NVG_PICK_DEBUG_LINE(A, B)
	#define NVG_PICK_DEBUG_VECTOR(A, D)
	#define NVG_PICK_DEBUG_VECTOR_SCALE(A, D, S)
#endif

struct NVGsegment {
	int firstPoint;				// Index into NVGpickScene::points
	short type;					// NVG_LINETO or NVG_BEZIERTO
	short flags;				// Flags relate to the corner between the prev segment and this one.
	float bounds[4];
	float startDir[2];			// Direction at t == 0
	float endDir[2];			// Direction at t == 1
	float miterDir[2];			// Direction of miter of corner between the prev segment and this one.
};
typedef struct NVGsegment NVGsegment;

struct NVGpickSubPath {
	short winding;				// TODO: Merge to flag field
	short closed;				// TODO: Merge to flag field
	
	int firstSegment;			// Index into NVGpickScene::segments
	int nsegments;

	float bounds[4];

	struct NVGpickSubPath* next;
};
typedef struct NVGpickSubPath NVGpickSubPath;

struct NVGpickPath {
	int id;
	short flags;
	short order;
	float strokeWidth;
	float miterLimit;
	short lineCap;
	short lineJoin;
	
	float bounds[4];
	int scissor; // Indexes into ps->points and defines scissor rect as XVec, YVec and Center

	struct NVGpickSubPath*	subPaths;
	struct NVGpickPath* next;
	struct NVGpickPath* cellnext;
};
typedef struct NVGpickPath NVGpickPath;

struct NVGpickScene {
	int npaths;

	NVGpickPath* paths;	// Linked list of paths
	NVGpickPath* lastPath; // The last path in the paths linked list (the first path added)
	NVGpickPath* freePaths; // Linked list of free paths

	NVGpickSubPath* freeSubPaths; // Linked list of free sub paths
	
	int width;
	int height;

	// Points for all path sub paths.
	float* points;
	int npoints;
	int cpoints;

	// Segments for all path sub paths
	NVGsegment* segments;
	int nsegments;
	int csegments;

	// Implicit quadtree
	float xdim;		// Width / (1 << nlevels)
	float ydim;		// Height / (1 << nlevels)
	int ncells;		// Total number of cells in all levels
	int nlevels;
	NVGpickPath*** levels;	// Index: [Level][LevelY * LevelW + LevelX] Value: Linked list of paths

	// Temp storage for picking
	int cpicked;
	NVGpickPath** picked;
};

static void nvg__segmentDir(NVGpickScene* ps, NVGpickSubPath* psp, NVGsegment* seg, float t, float d[2]);
static void nvg__bezierBounds(const float* points, float* bounds);
static void nvg__bezierInflections(const float* points, int coord, int* ninflections, float* inflections);
static void nvg__splitBezier(const float* points, float t, float* pointsA, float* pointsB);
static void nvg__smallsort(float* values, int n);
static void nvg__pickSceneInsert(NVGpickScene* ps, NVGpickPath* pp);
static NVGpickScene* nvg__pickSceneGet(NVGcontext* ctx);
static NVGpickPath* nvg__allocPickPath(NVGpickScene* ps);
static NVGpickSubPath* nvg__allocPickSubPath(NVGpickScene* ps);

//
// Bounds Utilities
//

static void nvg__initBounds(float bounds[4])
{
	bounds[0] = bounds[1] = 1e6f;
	bounds[2] = bounds[3] = -1e6f;
}

static void nvg__expandBounds(float bounds[4], const float *points, int npoints)
{
	int i;
	npoints *= 2;
	for (i = 0; i < npoints; i += 2) {
		bounds[0] = nvg__minf(bounds[0], points[i]);
		bounds[1] = nvg__minf(bounds[1], points[i+1]);
		bounds[2] = nvg__maxf(bounds[2], points[i]);
		bounds[3] = nvg__maxf(bounds[3], points[i+1]);
	}
}

static void nvg__unionBounds(float bounds[4], const float boundsB[4])
{
	bounds[0] = nvg__minf(bounds[0], boundsB[0]);
	bounds[1] = nvg__minf(bounds[1], boundsB[1]);
	bounds[2] = nvg__maxf(bounds[2], boundsB[2]);
	bounds[3] = nvg__maxf(bounds[3], boundsB[3]);
}

static void nvg__intersectBounds(float bounds[4], const float boundsB[4])
{
	bounds[0] = nvg__maxf(boundsB[0], bounds[0]);
	bounds[1] = nvg__maxf(boundsB[1], bounds[1]);
	bounds[2] = nvg__minf(boundsB[2], bounds[2]);
	bounds[3] = nvg__minf(boundsB[3], bounds[3]);
	
	bounds[2] = nvg__maxf(bounds[0], bounds[2]);
	bounds[3] = nvg__maxf(bounds[1], bounds[3]);
}

static int nvg__pointInBounds(float x, float y, const float bounds[4])
{
	if (x >= bounds[0] && x <= bounds[2] &&
		y >= bounds[1] && y <= bounds[3])
		return 1;
	else
		return 0;
}

//
// Building paths & sub paths
//

static int nvg__pickSceneAddPoints(NVGpickScene* ps, const float *xy, int n)
{
	int i;
	
	if (ps->npoints + n > ps->cpoints) {
		int cpoints = ps->npoints + n + (ps->cpoints << 1);
		float* points = realloc(ps->points, sizeof(float) * 2 * cpoints);
		if (points == NULL) return -1;
		ps->points = points;
		ps->cpoints = cpoints;
	}
	i = ps->npoints;
	if (xy != NULL)
		memcpy(&ps->points[i * 2], xy, sizeof(float) * 2 * n);
	ps->npoints += n;
	return i;
}

static void nvg__pickSubPathAddSegment(NVGpickScene* ps, NVGpickSubPath* psp, int firstPoint, int type, short flags)
{
	NVGsegment* seg = NULL;
	if (ps->nsegments == ps->csegments) {
		int csegments = 1 + ps->csegments + (ps->csegments << 1);
		NVGsegment* segments = (NVGsegment*)realloc(ps->segments, sizeof(NVGsegment) * csegments);
		if (segments == NULL) return;
		ps->segments = segments;
		ps->csegments = csegments;
	}
	
	if (psp->firstSegment == -1)
		psp->firstSegment = ps->nsegments;
		
	seg = &ps->segments[ps->nsegments];
	++ps->nsegments;
	seg->firstPoint = firstPoint;
	seg->type = (short)type;
	seg->flags = flags;
	++psp->nsegments;

	nvg__segmentDir(ps, psp, seg,  0, seg->startDir);
	nvg__segmentDir(ps, psp, seg,  1, seg->endDir);
}

static void nvg__segmentDir(NVGpickScene* ps, NVGpickSubPath* psp, NVGsegment* seg, float t, float d[2])
{
	float const* points = &ps->points[seg->firstPoint * 2];
	float omt, omt2, t2;
	float x0 = points[0*2+0], x1 = points[1*2+0], x2, x3;
	float y0 = points[0*2+1], y1 = points[1*2+1], y2, y3;
	
	switch(seg->type) {
	case NVG_LINETO:
		d[0] = x1 - x0;
		d[1] = y1 - y0;
		nvg__normalize(&d[0], &d[1]);
		break;
	case NVG_BEZIERTO:
		x2 = points[2*2+0];
		y2 = points[2*2+1];
		x3 = points[3*2+0];
		y3 = points[3*2+1];

		omt = 1.0f - t;
		omt2 = omt * omt;
		t2 = t * t;

		d[0] = 3.0f * omt2 * (x1 - x0) +
			6.0f * omt * t * (x2 - x1) +
			3.0f * t2 * (x3 - x2);
		d[1] = 3.0f * omt2 * (y1 - y0) +
			6.0f * omt * t * (y2 - y1) +
			3.0f * t2 * (y3 - y2);

		nvg__normalize(&d[0], &d[1]);
		break;
	}
}

static void nvg__pickSubPathAddFillSupports(NVGpickScene* ps, NVGpickSubPath* psp)
{
	NVGsegment* segments = &ps->segments[psp->firstSegment];
	int s;
	for (s = 0; s < psp->nsegments; ++s) {
		NVGsegment* seg = &segments[s];
		const float* points = &ps->points[seg->firstPoint * 2];
		
		if (seg->type == NVG_LINETO) {
			nvg__initBounds(seg->bounds);
			nvg__expandBounds(seg->bounds, points, 2);
		} else {
			nvg__bezierBounds(points, seg->bounds);
		}
	}
}

static void nvg__pickSubPathAddStrokeSupports(NVGpickScene* ps, NVGpickSubPath* psp, float strokeWidth, int lineCap, int lineJoin, float miterLimit)
{
	int closed = psp->closed;
	const float* points = ps->points;

	NVGsegment* seg = NULL;
	NVGsegment* segments = &ps->segments[psp->firstSegment];
	int nsegments = psp->nsegments;
	NVGsegment* prevseg = psp->closed ? &segments[psp->nsegments - 1] : NULL;
	
	int ns = 0; // nsupports
	float supportingPoints[32];
	int firstPoint, lastPoint;
	int s;

	if (closed == 0) {
		segments[0].flags |= NVG_PICK_CAP;
		segments[nsegments - 1].flags |= NVG_PICK_ENDCAP;
	}
	
	for (s = 0; s < nsegments; ++s) {
		seg = &segments[s];
		nvg__initBounds(seg->bounds);
		
		firstPoint = seg->firstPoint * 2;
		lastPoint = firstPoint + ((seg->type == NVG_LINETO) ? 2 : 6);
		
		ns = 0;
		
		// First two supporting points are either side of the start point
		supportingPoints[ns++] = points[firstPoint] - seg->startDir[1] * strokeWidth;
		supportingPoints[ns++] = points[firstPoint+1] + seg->startDir[0] * strokeWidth;
	
		supportingPoints[ns++] = points[firstPoint] + seg->startDir[1] * strokeWidth;
		supportingPoints[ns++] = points[firstPoint+1] - seg->startDir[0] * strokeWidth;

		// Second two supporting points are either side of the end point
		supportingPoints[ns++] = points[lastPoint] - seg->endDir[1] * strokeWidth;
		supportingPoints[ns++] = points[lastPoint+1] + seg->endDir[0] * strokeWidth;
	
		supportingPoints[ns++] = points[lastPoint] + seg->endDir[1] * strokeWidth;
		supportingPoints[ns++] = points[lastPoint+1] - seg->endDir[0] * strokeWidth;
		
		if (seg->flags & NVG_PICK_CORNER && prevseg != NULL) {
			float M2;
		
			seg->miterDir[0] = 0.5f * (-prevseg->endDir[1] - seg->startDir[1]);
			seg->miterDir[1] = 0.5f * (prevseg->endDir[0] + seg->startDir[0]);

			M2 = seg->miterDir[0] * seg->miterDir[0] + seg->miterDir[1] * seg->miterDir[1];
						
			if (M2 > 0.000001f) {
				float scale = 1.0f / M2;
				if (scale > 600.0f) {
					scale = 600.0f;
				}
				seg->miterDir[0] *= scale;
				seg->miterDir[1] *= scale;
			}

			NVG_PICK_DEBUG_VECTOR_SCALE(&points[firstPoint], seg->miterDir, 10);

			// Add an additional support at the corner on the other line
			supportingPoints[ns++] = points[firstPoint] - prevseg->endDir[1] * strokeWidth;
			supportingPoints[ns++] = points[firstPoint+1] + prevseg->endDir[0] * strokeWidth;

			if (lineJoin == NVG_MITER || lineJoin == NVG_BEVEL) {
				// Set a corner as beveled if the join type is bevel or mitered and
				// miterLimit is hit.
				if (lineJoin == NVG_BEVEL ||
					((M2 * miterLimit * miterLimit) < 1.0f)) {
					seg->flags |= NVG_PICK_BEVEL;
				} else {
					// Corner is mitered - add miter point as a support
					supportingPoints[ns++] = points[firstPoint] + seg->miterDir[0] * strokeWidth;
					supportingPoints[ns++] = points[firstPoint+1] + seg->miterDir[1] * strokeWidth;
				}
			} else if (lineJoin == NVG_ROUND) {
				// ... and at the midpoint of the corner arc

				float vertexN[2] = { -seg->startDir[0] + prevseg->endDir[0], -seg->startDir[1] + prevseg->endDir[1] };
				nvg__normalize(&vertexN[0], &vertexN[1]);
				
				supportingPoints[ns++] = points[firstPoint] + vertexN[0] * strokeWidth;
				supportingPoints[ns++] = points[firstPoint+1] + vertexN[1] * strokeWidth;
			}
		}
		
		if (seg->flags & NVG_PICK_CAP) {
			switch(lineCap) {
			case NVG_BUTT:
				// Supports for butt already added.
				break;
			case NVG_SQUARE:
				// Square cap supports are just the original two supports moved
				// out along the direction
				supportingPoints[ns++] = supportingPoints[0] - seg->startDir[0] * strokeWidth;
				supportingPoints[ns++] = supportingPoints[1] - seg->startDir[1] * strokeWidth;
				supportingPoints[ns++] = supportingPoints[2] - seg->startDir[0] * strokeWidth;
				supportingPoints[ns++] = supportingPoints[3] - seg->startDir[1] * strokeWidth;
				break;
			case NVG_ROUND:
				// Add one additional support for the round cap along the dir
				supportingPoints[ns++] = points[firstPoint] - seg->startDir[0] * strokeWidth;
				supportingPoints[ns++] = points[firstPoint+1] - seg->startDir[1] * strokeWidth;
				break;
			}
		}
		
		if (seg->flags & NVG_PICK_ENDCAP) {
			// End supporting points, either side of line
			int end = 4;

			switch(lineCap) {
			case NVG_BUTT:
				// Supports for butt already added.
				break;
			case NVG_SQUARE:
				// Square cap supports are just the original two supports moved
				// out along the direction
				supportingPoints[ns++] = supportingPoints[end + 0] + seg->endDir[0] * strokeWidth;
				supportingPoints[ns++] = supportingPoints[end + 1] + seg->endDir[1] * strokeWidth;
				supportingPoints[ns++] = supportingPoints[end + 2] + seg->endDir[0] * strokeWidth;
				supportingPoints[ns++] = supportingPoints[end + 3] + seg->endDir[1] * strokeWidth;
				break;
			case NVG_ROUND:
				// Add one additional support for the round cap along the dir
				supportingPoints[ns++] = points[lastPoint] + seg->endDir[0] * strokeWidth;
				supportingPoints[ns++] = points[lastPoint+1] + seg->endDir[1] * strokeWidth;
				break;
			}
		}
				
		nvg__expandBounds(seg->bounds, supportingPoints, ns / 2);
				
		prevseg = seg;
	}
}

static NVGpickPath* nvg__pickPathCreate(NVGcontext* context, int id, int forStroke)
{
	NVGpickScene* ps = nvg__pickSceneGet(context);

	int i = 0;
	
	int ncommands = context->ncommands;
	float* commands = context->commands;
	
	NVGpickPath* pp = NULL;
	NVGpickSubPath* psp = NULL;
	float start[2];
	int firstPoint;
	
	int hasHoles = 0;
	NVGpickSubPath* prev = NULL;

	float points[8];
	float inflections[2];
	int ninflections = 0;

	NVGstate* state = nvg__getState(context);
	float totalBounds[4];
	NVGsegment* segments = NULL;
	const NVGsegment* seg = NULL;
	NVGpickSubPath *curpsp;
	int s;

	pp = nvg__allocPickPath(ps);
	if (pp == NULL) return NULL;

	pp->id = id;
					
	while (i < ncommands) {
		int cmd = (int)commands[i];
		
		switch (cmd) {
		case NVG_MOVETO:
			start[0] = commands[i+1];
			start[1] = commands[i+2];

			// Start a new path for each sub path to handle sub paths that
			// intersect other sub paths.
			prev = psp;
			psp = nvg__allocPickSubPath(ps);
			if (psp == NULL) { psp = prev; break; }
			psp->firstSegment = -1;
			psp->winding = NVG_SOLID;
			psp->next = prev;

			nvg__pickSceneAddPoints(ps, &commands[i+1], 1);
			i += 3;
			break;
		case NVG_LINETO:
			firstPoint = nvg__pickSceneAddPoints(ps, &commands[i+1], 1);
			nvg__pickSubPathAddSegment(ps, psp, firstPoint - 1, cmd, NVG_PICK_CORNER);
			i += 3;
			break;
		case NVG_BEZIERTO:
			// Split the curve at it's dx==0 or dy==0 inflection points.
			// Thus:
			//		A horizontal line only ever interects the curves once.
			//	and
			//		Finding the closest point on any curve converges more reliably.
			
			// NOTE: We could just split on dy==0 here.
						
			memcpy(&points[0], &ps->points[(ps->npoints - 1) * 2], sizeof(float) * 2);
			memcpy(&points[2], &commands[i+1], sizeof(float) * 2 * 3);
							
			ninflections = 0;
			nvg__bezierInflections(points, 1, &ninflections, inflections);
			nvg__bezierInflections(points, 0, &ninflections, inflections);
			
			if (ninflections) {
				float previnfl = 0;
				float t;
				
				float pointsA[8], pointsB[8];

				int infl;

				nvg__smallsort(inflections, ninflections);

				for (infl = 0; infl < ninflections; ++infl) {
					if (nvg__absf(inflections[infl] - previnfl) < NVG_PICK_EPS)
						continue;
						
					t = (inflections[infl] - previnfl) * (1.0f / (1.0f - previnfl));
					
					previnfl = inflections[infl];
						
					nvg__splitBezier(points, t, pointsA, pointsB);

					firstPoint = nvg__pickSceneAddPoints(ps, &pointsA[2], 3);
					nvg__pickSubPathAddSegment(ps, psp, firstPoint - 1, cmd, (infl == 0) ? NVG_PICK_CORNER : 0);

					memcpy(points, pointsB, sizeof(float) * 8);
				}
				
				firstPoint = nvg__pickSceneAddPoints(ps, &pointsB[2], 3);
				nvg__pickSubPathAddSegment(ps, psp, firstPoint - 1, cmd, 0);
			} else {
				firstPoint = nvg__pickSceneAddPoints(ps, &commands[i+1], 3);
				nvg__pickSubPathAddSegment(ps, psp, firstPoint - 1, cmd, NVG_PICK_CORNER);
			}
			i += 7;
			break;
		case NVG_CLOSE:
			if (ps->points[(ps->npoints - 1) * 2] != start[0] ||
				ps->points[(ps->npoints - 1) * 2 + 1] != start[1]) {
				firstPoint = nvg__pickSceneAddPoints(ps, start, 1);
				nvg__pickSubPathAddSegment(ps, psp, firstPoint - 1, NVG_LINETO, NVG_PICK_CORNER);
			}
			psp->closed = 1;
			
			i++;
			break;
		case NVG_WINDING:
			psp->winding = (short)(int)commands[i+1];
			if (psp->winding == NVG_HOLE)
				hasHoles = 1;
			i += 2;
			break;
		default:
			i++;
			break;
		}
	}
	
	pp->flags = forStroke ? NVG_PICK_STROKE : NVG_PICK_FILL;
	pp->subPaths = psp;
	pp->strokeWidth = state->strokeWidth * 0.5f;
	pp->miterLimit = state->miterLimit;
	pp->lineCap = (short)state->lineCap;
	pp->lineJoin = (short)state->lineJoin;
	
	nvg__initBounds(totalBounds);

	for (curpsp = psp; curpsp; curpsp = curpsp->next) {
		if (forStroke)
			nvg__pickSubPathAddStrokeSupports(ps, curpsp, pp->strokeWidth, pp->lineCap, pp->lineJoin, pp->miterLimit);
		else
			nvg__pickSubPathAddFillSupports(ps, curpsp);
		
		segments = &ps->segments[curpsp->firstSegment];
		nvg__initBounds(curpsp->bounds);
		for (s = 0; s < curpsp->nsegments; ++s) {
			seg = &segments[s];
			NVG_PICK_DEBUG_BOUNDS(seg->bounds);
			nvg__unionBounds(curpsp->bounds, seg->bounds);
		}

		nvg__unionBounds(totalBounds, curpsp->bounds);
	}
	
	// Store the scissor rect if present.
	if (state->scissor.extent[0] != -1.0f) {
		// Use points storage to store the scissor data
		float* scissor = NULL;
		pp->scissor = nvg__pickSceneAddPoints(ps, NULL, 4);
		scissor = &ps->points[pp->scissor*2];
	
		memcpy(scissor, state->scissor.xform, 6 * sizeof(float));
		memcpy(scissor + 6, state->scissor.extent, 2 * sizeof(float));
		
		pp->flags |= NVG_PICK_SCISSOR;
	}
				
	memcpy(pp->bounds, totalBounds, sizeof(float) * 4);
	
	return pp;
}

// Struct management

static NVGpickPath* nvg__allocPickPath(NVGpickScene* ps)
{
	NVGpickPath* pp = ps->freePaths;
	if (pp) {
		ps->freePaths = pp->next;
	} else {
		pp = (NVGpickPath*)malloc(sizeof(NVGpickPath));
	}
	memset(pp, 0, sizeof(NVGpickPath));
	return pp;
}

// Return a pick path and any sub paths to the free lists
static void nvg__freePickPath(NVGpickScene* ps, NVGpickPath* pp)
{
	// Add all sub paths to the sub path free list.
	// Finds the end of the path sub paths, links that to the current
	// sub path free list head and replaces the head ptr with the
	// head path sub path entry.
	NVGpickSubPath* psp = NULL;
	for (psp = pp->subPaths; psp && psp->next; psp = psp->next) {
	}

	if (psp) {
		psp->next = ps->freeSubPaths;
		ps->freeSubPaths = pp->subPaths;
	}
	pp->subPaths = NULL;
	
	// Add the path to the path freelist
	pp->next = ps->freePaths;
	ps->freePaths = pp;
	if (pp->next == NULL)
		ps->lastPath = pp;
}

static NVGpickSubPath* nvg__allocPickSubPath(NVGpickScene* ps)
{
	NVGpickSubPath* psp = ps->freeSubPaths;
	if (psp) {
		ps->freeSubPaths = psp->next;
	} else {
		psp = (NVGpickSubPath*)malloc(sizeof(NVGpickSubPath));
		if (psp == NULL) return NULL;
	}
	memset(psp, 0, sizeof(NVGpickSubPath));
	return psp;
}

static void nvg__returnPickSubPath(NVGpickScene* ps, NVGpickSubPath* psp)
{
	psp->next = ps->freeSubPaths;
	ps->freeSubPaths = psp;
}

static NVGpickScene* nvg__allocPickScene()
{
	NVGpickScene* ps = (NVGpickScene*)malloc(sizeof(NVGpickScene));
	if (ps == NULL) return NULL;
	memset(ps, 0, sizeof(NVGpickScene));
	ps->nlevels = 5;
	return ps;
}

void nvg__deletePickScene(NVGpickScene* ps)
{
	NVGpickPath* pp;
	NVGpickSubPath* psp;
	
	// Add all paths (and thus sub paths)  to the free list(s).
	while(ps->paths) {
		pp = ps->paths->next;
		nvg__freePickPath(ps, ps->paths);
		ps->paths = pp;
	}
	
	// Delete all paths
	while(ps->freePaths) {
		pp = ps->freePaths;
		ps->freePaths = pp->next;

		while (pp->subPaths) {
			psp = pp->subPaths;
			pp->subPaths = psp->next;
			free(psp);
		}
		free(pp);
	}

	// Delete all sub paths
	while(ps->freeSubPaths) {
		psp = ps->freeSubPaths->next;
		free(ps->freeSubPaths);
		ps->freeSubPaths = psp;
	}
	
	ps->npoints = 0;
	ps->nsegments = 0;
	
	if (ps->levels) {
		free(ps->levels[0]);
		free(ps->levels);
	}
	
	if (ps->picked)
		free(ps->picked);
	if (ps->points)
		free(ps->points);
	if (ps->segments)
		free(ps->segments);
		
	free(ps);
}

static NVGpickScene* nvg__pickSceneGet(NVGcontext* ctx)
{
	if (ctx->pickScene == NULL)
		ctx->pickScene = nvg__allocPickScene();
	
	return ctx->pickScene;
}

// Marks the fill of the current path as pickable with the specified id.
void nvgFillHitRegion(NVGcontext* ctx, int id)
{
	NVGpickScene* ps = nvg__pickSceneGet(ctx);
	
	NVGpickPath* pp = nvg__pickPathCreate(ctx, id, 0);
	
	nvg__pickSceneInsert(ps, pp);
}

// Marks the stroke of the current path as pickable with the specified id.
void nvgStrokeHitRegion(NVGcontext* ctx, int id)
{
	NVGpickScene* ps = nvg__pickSceneGet(ctx);
	
	NVGpickPath* pp = nvg__pickPathCreate(ctx, id, 1);
		
	nvg__pickSceneInsert(ps, pp);
}

// Applies Casteljau's algorithm to a cubic bezier for a given parameter t
// points is 4 points (8 floats)
// lvl1 is 3 points (6 floats)
// lvl2 is 2 points (4 floats)
// lvl3 is 1 point (2 floats)
static void nvg__casteljau(const float* points, float t, float* lvl1, float* lvl2, float* lvl3)
{
	int x0 = 0*2+0, x1 = 1*2+0, x2 = 2*2+0, x3 = 3*2+0;
	int y0 = 0*2+1, y1 = 1*2+1, y2 = 2*2+1, y3 = 3*2+1;

	// Level 1
	lvl1[x0] = (points[x1] - points[x0]) * t + points[x0];
	lvl1[y0] = (points[y1] - points[y0]) * t + points[y0];

	lvl1[x1] = (points[x2] - points[x1]) * t + points[x1];
	lvl1[y1] = (points[y2] - points[y1]) * t + points[y1];

	lvl1[x2] = (points[x3] - points[x2]) * t + points[x2];
	lvl1[y2] = (points[y3] - points[y2]) * t + points[y2];

	// Level 2
	lvl2[x0] = (lvl1[x1] - lvl1[x0]) * t + lvl1[x0];
	lvl2[y0] = (lvl1[y1] - lvl1[y0]) * t + lvl1[y0];

	lvl2[x1] = (lvl1[x2] - lvl1[x1]) * t + lvl1[x1];
	lvl2[y1] = (lvl1[y2] - lvl1[y1]) * t + lvl1[y1];

	// Level 3
	lvl3[x0] = (lvl2[x1] - lvl2[x0]) * t + lvl2[x0];
	lvl3[y0] = (lvl2[y1] - lvl2[y0]) * t + lvl2[y0];
}

// Calculates a point on a bezier at point t.
static void nvg__bezierEval(const float* points, float t, float* tpoint)
{
	float omt = 1 - t;
	
	float omt3 = omt * omt * omt;
	float omt2 = omt * omt;
	float t3 = t * t * t;
	float t2 = t * t;
	
	tpoint[0] = points[0] * omt3 +
				points[2] * 3.0f * omt2 * t +
				points[4] * 3.0f * omt * t2 +
				points[6] * t3;

	tpoint[1] = points[1] * omt3 +
				points[3] * 3.0f * omt2 * t +
				points[5] * 3.0f * omt * t2 +
				points[7] * t3;
}

// Splits a cubic bezier curve into two parts at point t.
static void nvg__splitBezier(const float* points, float t, float* pointsA, float* pointsB)
{
	int x0 = 0*2+0, x1 = 1*2+0, x2 = 2*2+0, x3 = 3*2+0;
	int y0 = 0*2+1, y1 = 1*2+1, y2 = 2*2+1, y3 = 3*2+1;

	float lvl1[6], lvl2[4], lvl3[2];
	
	nvg__casteljau(points, t, lvl1, lvl2, lvl3);

	// First half
	pointsA[x0] = points[x0];
	pointsA[y0] = points[y0];
	
	pointsA[x1] = lvl1[x0];
	pointsA[y1] = lvl1[y0];
	
	pointsA[x2] = lvl2[x0];
	pointsA[y2] = lvl2[y0];

	pointsA[x3] = lvl3[x0];
	pointsA[y3] = lvl3[y0];

	// Second half
	pointsB[x0] = lvl3[x0];
	pointsB[y0] = lvl3[y0];
	
	pointsB[x1] = lvl2[x1];
	pointsB[y1] = lvl2[y1];
	
	pointsB[x2] = lvl1[x2];
	pointsB[y2] = lvl1[y2];

	pointsB[x3] = points[x3];
	pointsB[y3] = points[y3];
}

// Calculates the inflection points in coordinate coord (X = 0, Y = 1) of a cubic bezier.
// Appends any found inflection points to the array inflections and increments *ninflections.
// So finds the parameters where dx/dt or dy/dt is 0
static void nvg__bezierInflections(const float* points, int coord, int* ninflections, float* inflections)
{
	float v0 = points[0*2+coord], v1 = points[1*2+coord], v2 = points[2*2+coord], v3 = points[3*2+coord];
	float t[2];
	float a,b,c,d;
	int nvalid = *ninflections;
	
	a = 3.0f * ( -v0 + 3.0f * v1 - 3.0f * v2 + v3 );
	b = 6.0f * ( v0 - 2.0f * v1 + v2 );
	c = 3.0f * ( v1 - v0 );

	d = b*b - 4.0f * a * c;
	if ( nvg__absf(d - 0.0f) < NVG_PICK_EPS) {
		// Zero or one root
		t[0] = -b / 2.0f * a;
		if (t[0] > NVG_PICK_EPS && t[0] < (1.0f - NVG_PICK_EPS)) {
			inflections[nvalid] = t[0];
			++nvalid;
		}
	} else if (d > NVG_PICK_EPS) {
		int i;

		// zero, one or two roots
		d = nvg__sqrtf(d);
		
		t[0] = (-b + d) / (2.0f * a);
		t[1] = (-b - d) / (2.0f * a);
		
		for (i = 0; i < 2; ++i) {
			if (t[i] > NVG_PICK_EPS && t[i] < (1.0f - NVG_PICK_EPS)) {
				inflections[nvalid] = t[i];
				++nvalid;
			}
		}
	} else {
		// zero roots
	}
	
	*ninflections = nvalid;
}

// Sort a small number of floats in ascending order (0 < n < 6)
static void nvg__smallsort(float* values, int n)
{
	float tmp;
	int bSwapped = 1;
	int i,j;
	
	for (j = 0; (j < n - 1) && bSwapped;++j) {
		bSwapped = 0;
		for (i = 0; i < n - 1; ++i) {
			if (values[i] > values[i+1]) {
				tmp = values[i];
				values[i] = values[i+1];
				values[i+1] = tmp;
			}
		}
	}
}

// Calculates the bounding rect of a given cubic bezier curve.
static void nvg__bezierBounds(const float* points, float* bounds)
{
	float inflections[4];
	int ninflections = 0;
	float tpoint[2];
	int i;

	nvg__initBounds(bounds);

	// Include start and end points in bounds
	nvg__expandBounds(bounds, &points[0], 1);
	nvg__expandBounds(bounds, &points[6], 1);

	// Calculate dx==0 and dy==0 inflection points and add then
	// to the bounds
	
	nvg__bezierInflections(points, 0, &ninflections, inflections);
	nvg__bezierInflections(points, 1, &ninflections, inflections);

	for (i = 0; i < ninflections; ++i) {
		nvg__bezierEval(points, inflections[i], tpoint);
		nvg__expandBounds(bounds, tpoint, 1);
	}
}

// Checks to see if a line originating from x,y along the +ve x axis
// intersects the given line (points[0],points[1]) -> (points[2], points[3]).
// Returns 1 on intersection, 0 on no intersection.
// Horizontal lines are never hit.
static int nvg__intersectLine(const float* points, float x, float y)
{
	float x1 = points[0];
	float y1 = points[1];
	float x2 = points[2];
	float y2 = points[3];
	
	float d = y2 - y1;
	float s, lineX;
	
	if ( d > NVG_PICK_EPS || d < -NVG_PICK_EPS ) {
		s = (x2 - x1) / d;
		lineX = x1 + (y - y1) * s;
		return lineX > x;
	}
	else return 0;
}

// Checks to see if a line originating from x,y along the +ve x axis
// intersects the given bezier.
// It is assumed that the line originates from within the bounding box of
// the bezier and that the curve has no dy=0 inflection points.
// Returns the number of intersections found (which is either 1 or 0)
static int nvg__intersectBezier(const float* points, float x, float y)
{
	float x0 = points[0*2+0], x1 = points[1*2+0], x2 = points[2*2+0], x3 = points[3*2+0];
	float y0 = points[0*2+1], y1 = points[1*2+1], y2 = points[2*2+1], y3 = points[3*2+1];

	float t;
	float ty;
	float dty;
	float omt, omt2, omt3, t2, t3;

	if (y0 == y1 && y1 == y2 && y2 == y3) return 0;

	// Initial t guess
	if (y3 != y0) t = (y - y0) / (y3 - y0);
	else if (x3 != x0) t = (x - x0) / (x3 - x0);
	else t = 0.5f;

	// A few Newton iterations
	int iter;
	for (iter = 0; iter < 6; ++iter) {
		omt = 1 - t;
		omt2 = omt * omt;
		t2 = t * t;
		omt3 = omt2 * omt;
		t3 = t2 * t;
		
		ty = y0 * omt3 +
			y1 * 3.0f * omt2 * t +
			y2 * 3.0f * omt * t2 +
			y3 * t3;

		// Newton iteration
		dty = 3.0f * omt2 * (y1 - y0) +
			6.0f * omt * t * (y2 - y1) +
			3.0f * t2 * (y3 - y2);

		// dty will never == 0 since:
		//  Either omt, omt2 are zero OR t2 is zero
		//	y0 != y1 != y2 != y3 (checked above)
		t = t - ( ty - y ) / dty;
 	}
 	
	omt = 1 - t;
	omt2 = omt * omt;
	t2 = t * t;
	omt3 = omt2 * omt;
	t3 = t2 * t;
	float tx;
	
	tx = x0 * omt3 +
		x1 * 3.0f * omt2 * t +
		x2 * 3.0f * omt * t2 +
		x3 * t3;

	if (tx > x)
		return 1;
	else
		return 0;
}

// Finds the closest point on a line to a given point
static void nvg__closestLine(const float* points, float x, float y, float* closest, float* ot)
{
	float x1 = points[0];
	float y1 = points[1];
	float x2 = points[2];
	float y2 = points[3];
	float pqx = x2 - x1;
	float pqz = y2 - y1;
	float dx = x - x1;
	float dz = y - y1;
	float d = pqx*pqx + pqz*pqz;
	float t = pqx*dx + pqz*dz;
	if (d > 0) t /= d;
	if (t < 0) t = 0;
	else if (t > 1) t = 1;
	closest[0] = x1 + t*pqx;
	closest[1] = y1 + t*pqz;
	*ot = t;
}

// Finds the closest point on a curve for a given point (x,y).
// Assumes that the curve has no dx==0 or dy==0 inflection points.
static void nvg__closestBezier(const float* points, float x, float y, float* closest, float *ot)
{
	float x0 = points[0*2+0], x1 = points[1*2+0], x2 = points[2*2+0], x3 = points[3*2+0];
	float y0 = points[0*2+1], y1 = points[1*2+1], y2 = points[2*2+1], y3 = points[3*2+1];

	// This assumes that the curve has no dy=0 inflection points.
	
	// Initial t guess
	float t = 0.5f;
	float ty, dty, ddty, errory;
	float tx, dtx, ddtx, errorx;
	float omt, omt2, omt3, t2, t3;
	float n, d;

	// A few Newton iterations
	int iter;
	for (iter = 0; iter < 6; ++iter) {
		omt = 1 - t;
		omt2 = omt * omt;
		t2 = t * t;
		omt3 = omt2 * omt;
		t3 = t2 * t;
		
		ty = y0 * omt3 +
			y1 * 3.0f * omt2 * t +
			y2 * 3.0f * omt * t2 +
			y3 * t3;

		tx = x0 * omt3 +
			x1 * 3.0f * omt2 * t +
			x2 * 3.0f * omt * t2 +
			x3 * t3;

		// Newton iteration
		dty = 3.0f * omt2 * (y1 - y0) +
			6.0f * omt * t * (y2 - y1)+
			3.0f * t2 * (y3 - y2);

		ddty = 6.0f * omt * (y2 - 2.0f * y1 + y0) +
			6.0f * t * (y3 - 2.0f * y2 + y1);

		dtx = 3.0f * omt2 * (x1 - x0) +
			6.0f * omt * t * (x2 - x1) +
			3.0f * t2 * (x3 - x2);

		ddtx = 6.0f * omt * (x2 - 2.0f * x1 + x0) +
			6.0f * t * (x3 - 2.0f * x2 + x1);
				
		errorx = tx - x;
		errory = ty - y;

		n = errorx * dtx + errory * dty;
		if (n == 0.0f) break;
			
		d = dtx * dtx + dty * dty + errorx * ddtx + errory * ddty;
		if (d != 0.0f) t = t - n / d;
		else break;
 	}

 	t = nvg__maxf(0, nvg__minf(1.0 ,t));
 	
 	*ot = t;
 	
	omt = 1 - t;
	omt2 = omt * omt;
	t2 = t * t;
	omt3 = omt2 * omt;
	t3 = t2 * t;

	ty = y0 * omt3 +
		y1 * 3.0f * omt2 * t +
		y2 * 3.0f * omt * t2 +
		y3 * t3;

	tx = x0 * omt3 +
		x1 * 3.0f * omt2 * t +
		x2 * 3.0f * omt * t2 +
		x3 * t3;

	closest[0] = tx;
	closest[1] = ty;
}


// Returns:
//	1	If (x,y) is contained by the stroke of the path
//	0	If (x,y) is not contained by the path.
static int nvg__pickSubPathStroke(const NVGpickScene* ps, const NVGpickSubPath* psp, float x, float y, float strokeWidth, int lineCap, int lineJoin)
{
	int nsegments;
	const NVGsegment * seg;
	float closest[2];
	float t;
	
	float distSqd = 0;
	float d[2];
	float strokeWidthSqd;

	const NVGsegment* prevseg;
	int s;
	
	if (nvg__pointInBounds(x, y, psp->bounds) == 0)
		return 0;

	// Trace a line from x,y out along the positive x axis and count the
	// number of intersections.
	nsegments = psp->nsegments;
	seg = ps->segments + psp->firstSegment;
	
	strokeWidthSqd = strokeWidth * strokeWidth;
	
	prevseg = psp->closed ? &ps->segments[psp->firstSegment + nsegments - 1] : NULL;
		
	for (s = 0; s < nsegments; ++s, prevseg=seg, ++seg) {
		if (nvg__pointInBounds(x, y, seg->bounds) != 0) {
			// Line potentially hits stroke.
			switch(seg->type) {
			case NVG_LINETO:
				nvg__closestLine(&ps->points[seg->firstPoint * 2], x, y, closest, &t);
				break;
	
			case NVG_BEZIERTO:
				nvg__closestBezier(&ps->points[seg->firstPoint * 2], x, y, closest, &t);
				break;
	
			default:
					continue;
			}

			d[0] = x - closest[0];
			d[1] = y - closest[1];

			if ((t >= NVG_PICK_EPS && t <= (1.0f - NVG_PICK_EPS)) ||
				!(seg->flags & (NVG_PICK_CORNER | NVG_PICK_CAP | NVG_PICK_ENDCAP)) ||
				(lineJoin == NVG_ROUND)) {
				// Closest point is in the middle of the line/curve, at a rounded join/cap
				// or at a smooth join
				distSqd = d[0] * d[0] + d[1] * d[1];
				if (distSqd < strokeWidthSqd)
					return 1;
			} else if ( ( (t > (1.0f - NVG_PICK_EPS)) && (seg->flags & NVG_PICK_ENDCAP)) ||
						( (t < NVG_PICK_EPS) && (seg->flags & NVG_PICK_CAP) ) ) {
				float dirD;
				switch(lineCap) {
				case NVG_BUTT:
					distSqd = d[0] * d[0] + d[1] * d[1];
					if (t < NVG_PICK_EPS)
						dirD = -(d[0] * seg->startDir[0] + d[1] * seg->startDir[1]);
					else
						dirD = d[0] * seg->endDir[0] + d[1] * seg->endDir[1];

					if (dirD < -NVG_PICK_EPS && distSqd < strokeWidthSqd)
						return 1;
					break;
				case NVG_SQUARE:
					if ( nvg__absf(d[0]) < strokeWidth && nvg__absf(d[1]) < strokeWidth)
						return 1;
					break;
				case NVG_ROUND:
					distSqd = d[0] * d[0] + d[1] * d[1];
					if (distSqd < strokeWidthSqd)
						return 1;
					break;
				}
			} else if (seg->flags & NVG_PICK_CORNER) {
				// Closest point is at a corner

				const NVGsegment* seg0;
				const NVGsegment* seg1;
				
				if (t < NVG_PICK_EPS) {
					seg0 = prevseg;
					seg1 = seg;
				} else {
					seg0 = seg;
					seg1 = (s == (nsegments-1)) ? &ps->segments[psp->firstSegment] : (seg+1);
				}

				if (!(seg1->flags & NVG_PICK_BEVEL)) {
					float prevNDist, curNDist;
				
					prevNDist = -seg0->endDir[1] * d[0] + seg0->endDir[0] * d[1];
					curNDist = seg1->startDir[1] * d[0] - seg1->startDir[0] * d[1];
					
					if (nvg__absf(prevNDist) < strokeWidth &&
						nvg__absf(curNDist) < strokeWidth) {
						return 1;
					}
				} else {
					d[0] -= -seg1->startDir[1] * strokeWidth;
					d[1] -= +seg1->startDir[0] * strokeWidth;
												
					if ( (seg1->miterDir[0] * d[0] + seg1->miterDir[1] * d[1]) < 0.0f )
						return 1;
				}
			}
		}
	}
	
	return 0;
	
}

// Returns:
//	1	If (x,y) is contained by the path and the path is solid.
//	-1 	If (x,y) is contained by the path and the path is a hole.
//	0	If (x,y) is not contained by the path.
static int nvg__pickSubPath(const NVGpickScene* ps, const NVGpickSubPath* psp, float x, float y)
{
	int nsegments = psp->nsegments;
	NVGsegment const* seg = &ps->segments[psp->firstSegment];
	int nintersections = 0;
	int s;

	if (nvg__pointInBounds(x, y, psp->bounds) == 0)
		return 0;

	// Trace a line from x,y out along the positive x axis and count the
	// number of intersections.
			
	for (s = 0; s < nsegments; ++s, ++seg) {
		if ((seg->bounds[1] - NVG_PICK_EPS) < y &&
			(seg->bounds[3] - NVG_PICK_EPS) > y &&
			seg->bounds[2] > x) {
			// Line hits the box.
			switch(seg->type) {
			case NVG_LINETO:
				if (seg->bounds[0] > x)
					// Line originates outside the box.
					++nintersections;
				else
					// Line originates inside the box.
					nintersections += nvg__intersectLine(&ps->points[seg->firstPoint * 2], x, y);
				break;
	
			case NVG_BEZIERTO:
				if (seg->bounds[0] > x)
					// Line originates outside the box.
					++nintersections;
				else
					// Line originates inside the box.
					nintersections += nvg__intersectBezier(&ps->points[seg->firstPoint * 2], x, y);
				break;
	
			default:
				break;
			}
		}
	}

	if (nintersections & 1)
		return (psp->winding == NVG_SOLID) ? 1 : -1;
	else
		return 0;
}

static int nvg__pickPath(const NVGpickScene* ps, const NVGpickPath* pp, float x, float y)
{
	int pickCount = 0;
	NVGpickSubPath* psp = pp->subPaths;
	while(psp) {
		pickCount += nvg__pickSubPath(ps, psp, x, y);
		psp = psp->next;
	}
	
	return (pickCount != 0) ? 1 : 0;
}


static int nvg__pickPathStroke(const NVGpickScene* ps, const NVGpickPath* pp, float x, float y)
{
	NVGpickSubPath* psp = pp->subPaths;
	while(psp) {
		if (nvg__pickSubPathStroke(ps, psp, x, y, pp->strokeWidth, pp->lineCap, pp->lineJoin))
			return 1;
		psp = psp->next;
	}

	return 0;
}

static int nvg__comparePaths(const void* a, const void* b)
{
	NVGpickPath* pathA = *(NVGpickPath**)a;
	NVGpickPath* pathB = *(NVGpickPath**)b;
	
	return pathB->order - pathA->order;
}

static int nvg__pickPathTestBounds(const NVGpickScene* ps, const NVGpickPath* pp, float x, float y)
{
	if (nvg__pointInBounds(x, y, pp->bounds) != 0) {
		if (pp->flags & NVG_PICK_SCISSOR) {
			const float* scissor = &ps->points[pp->scissor*2];
			float rx = x - scissor[4];
			float ry = y - scissor[5];
								
			if ( nvg__absf((scissor[0] * rx) + (scissor[1] * ry)) > scissor[6] ||
				 nvg__absf((scissor[2] * rx) + (scissor[3] * ry)) > scissor[7])
				return 0;
		}

		return 1;
	}
	
	return 0;
}

// Fills ids with a list of the top most maxids ids under the specified position.
// Returns the number of ids written into ids, up to maxids.
int nvgHitTestAll(NVGcontext* ctx, float x, float y, int flags, int* ids, int maxids)
{
	NVGpickScene* ps;
	int npicked = 0;
	int hit;
	int lvl;
	int id;
	int levelwidth;
	int cellx, celly;

	if (ctx->pickScene == NULL)
		return 0;

	ps = ctx->pickScene;
	levelwidth = 1 << (ps->nlevels - 1);
	cellx = nvg__clampi((int)(x / ps->xdim), 0, levelwidth);
	celly = nvg__clampi((int)(y / ps->ydim), 0, levelwidth);

	for (lvl = ps->nlevels - 1; lvl >= 0; --lvl) {
		NVGpickPath* pp = ps->levels[lvl][celly*levelwidth + cellx];
		while(pp) {
			if (nvg__pickPathTestBounds(ps, pp, x, y)) {
			
				hit = 0;
				if ((flags & NVG_TEST_STROKE) && (pp->flags & NVG_PICK_STROKE))
					hit = nvg__pickPathStroke(ps, pp, x, y);

				if (!hit && (flags & NVG_TEST_FILL) && (pp->flags & NVG_PICK_FILL))
					hit = nvg__pickPath(ps, pp, x, y);

				if (hit) {
					if (npicked == ps->cpicked) {
						int cpicked = ps->cpicked + ps->cpicked;
						NVGpickPath** picked = realloc(ps->picked, sizeof(NVGpickPath*) * ps->cpicked);
						if (picked == NULL)
							break;
						ps->cpicked = cpicked;
						ps->picked = picked;
					}
					ps->picked[npicked] = pp;
					++npicked;
				}
			}
			pp = pp->next;
		}
		
		cellx >>= 1;
		celly >>= 1;
		levelwidth >>= 1;
	}
	
	qsort(ps->picked, npicked, sizeof(NVGpickPath*), nvg__comparePaths);
	
	maxids = nvg__mini(maxids, npicked);
	for (id = 0; id < maxids; ++id)
		ids[id] = ps->picked[id]->id;
		
	return maxids;
}

// Returns the id of the pickable shape containing x,y or -1 if not shape is found.
int nvgHitTest(NVGcontext* ctx, float x, float y, int flags)
{
	NVGpickScene* ps;
	int levelwidth;
	int cellx, celly;
	int bestOrder = -1;
	int bestID = -1;
	int hit = 0;
	int lvl;

	if (ctx->pickScene == NULL)
		return -1;

	ps = ctx->pickScene;

	levelwidth = 1 << (ps->nlevels - 1);
	cellx = nvg__clampi((int)(x / ps->xdim), 0, levelwidth-1);
	celly = nvg__clampi((int)(y / ps->ydim), 0, levelwidth-1);

	for (lvl = ps->nlevels - 1; lvl >= 0; --lvl) {
		NVGpickPath* pp = ps->levels[lvl][celly*levelwidth + cellx];
		while(pp) {
			if (nvg__pickPathTestBounds(ps, pp, x, y)) {
				
				if ((flags & NVG_TEST_STROKE) && (pp->flags & NVG_PICK_STROKE))
					hit = nvg__pickPathStroke(ps, pp, x, y);

				if (!hit && (flags & NVG_TEST_FILL) && (pp->flags & NVG_PICK_FILL))
					hit = nvg__pickPath(ps, pp, x, y);
					
				if (hit) {
					if (pp->order > bestOrder) {
						bestOrder = pp->order;
						bestID = pp->id;
					}
				}
			}
			pp = pp->next;
		}
		
		cellx >>= 1;
		celly >>= 1;
		levelwidth >>= 1;
	}
	
	return bestID;
}

int nvgInFill(NVGcontext* ctx, float x, float y)
{
	NVGpickScene* ps = nvg__pickSceneGet(ctx);

	int oldnpoints = ps->npoints;
	int oldnsegments = ps->nsegments;

	NVGpickPath* pp = nvg__pickPathCreate(ctx, 1, 0);
	int hit;

	if (nvg__pointInBounds(x, y, pp->bounds) == 0)
		return 0;
	
	hit = nvg__pickPath(ps, pp, x, y);

	nvg__freePickPath(ps, pp);

	ps->npoints = oldnpoints;
	ps->nsegments = oldnsegments;
		
	return hit;
}

int nvgInStroke(NVGcontext* ctx, float x, float y)
{
	NVGpickScene* ps = nvg__pickSceneGet(ctx);

	int oldnpoints = ps->npoints;
	int oldnsegments = ps->nsegments;

	NVGpickPath* pp = nvg__pickPathCreate(ctx, 1, 1);
	int hit;

	if (nvg__pointInBounds(x, y, pp->bounds) == 0)
		return 0;
	
	hit = nvg__pickPathStroke(ps, pp, x, y);

	nvg__freePickPath(ps, pp);

	ps->npoints = oldnpoints;
	ps->nsegments = oldnsegments;
		
	return hit;
}


static int nvg__countBitsUsed(int v)
{
	if (v == 0)
		return 0;
#if defined(__clang__) || defined(__GNUC__)
	return (sizeof(int) * 8) - __builtin_clz(v);
#elif defined(_MSC_VER)
	return (sizeof(int) * 8) -  __lzcnt(v);
#else
	// Poor mans 16bit clz variant
	int test;
	int nbits;

	if (v & 0xF000) {
		test = 1 << 15;
		nbits = 16;
	} else if (v & 0x0F00) {
		test = 1 << 11;
		nbits = 12;
	} else if (v & 0x00F0) {
		test = 1 << 7;
		nbits = 8;
	} else if (v & 0x000F) {
		test = 1 << 3;
		nbits = 4;
	} else {
		return 0;
	}

	while(nbits) {
		if (v & test)
			return nbits;
		--nbits;
		test >>= 1;
	}
	
	return 0;
#endif
}

static void nvg__pickSceneInsert(NVGpickScene* ps, NVGpickPath* pp)
{
	int cellbounds[4];
	int base = ps->nlevels - 1;
	int level;
	int levelwidth;
	int levelshift;
	int levelx;
	int levely;
	NVGpickPath** cell = NULL;

	// Bit tricks for inserting into an implicit quadtree.
		
	// Calc bounds of path in cells at the lowest level
	cellbounds[0] = (int)(pp->bounds[0] / ps->xdim);
	cellbounds[1] = (int)(pp->bounds[1] / ps->ydim);
	cellbounds[2] = (int)(pp->bounds[2] / ps->xdim);
	cellbounds[3] = (int)(pp->bounds[3] / ps->ydim);

	// Find which bits differ between the min/max x/y coords
	cellbounds[0] ^= cellbounds[2];
	cellbounds[1] ^= cellbounds[3];
		
	// Use the number of bits used (countBitsUsed(x) == sizeof(int) * 8 - clz(x);
	// to calculate the level to insert at (the level at which the bounds fit in a
	// single cell)
	level = nvg__mini(base - nvg__countBitsUsed( cellbounds[0] ), base - nvg__countBitsUsed( cellbounds[1] ) );
	if (level < 0)
		level = 0;

	// Find the correct cell in the chosen level, clamping to the edges.
	levelwidth = 1 << level;
	levelshift = (ps->nlevels - level) - 1;
	levelx = nvg__clampi(cellbounds[2] >> levelshift, 0, levelwidth - 1);
	levely = nvg__clampi(cellbounds[3] >> levelshift, 0, levelwidth - 1);

	// Insert the path into the linked list at that cell.
	cell = &ps->levels[level][levely * levelwidth + levelx];

	pp->cellnext = *cell;
	*cell = pp;

	if (ps->paths == NULL)
		ps->lastPath = pp;
	pp->next = ps->paths;
	ps->paths = pp;

	// Store the order (depth) of the path for picking ops.
	pp->order = (short)ps->npaths;
	++ps->npaths;
}

void nvg__pickBeginFrame(NVGcontext* ctx, int width, int height)
{
	NVGpickScene* ps = nvg__pickSceneGet(ctx);
	float lowestSubDiv;

	NVG_PICK_DEBUG_NEWFRAME();

	// Return all paths & sub paths from last frame to the free list
	while(ps->paths) {
		NVGpickPath* pp = ps->paths->next;
		nvg__freePickPath(ps, ps->paths);
		ps->paths = pp;
	}
	
	ps->paths = NULL;
	ps->npaths = 0;
		
	// Store the screen metrics for the quadtree
	ps->width = width;
	ps->height = height;
	
	lowestSubDiv = (float)(1 << (ps->nlevels - 1));
	ps->xdim = (float)width / lowestSubDiv;
	ps->ydim = (float)height / lowestSubDiv;
	
	// Allocate the quadtree if required.
	if (ps->levels == NULL) {
		int ncells = 1;
		int leveldim;
		int l, cell;
		
		ps->levels = (NVGpickPath***)malloc(sizeof(NVGpickPath**) * ps->nlevels);
		for (l = 0; l < ps->nlevels; ++l) {
			leveldim = 1 << l;
			
			ncells += leveldim * leveldim;
		}
			
		ps->levels[0] = (NVGpickPath**)malloc(sizeof(NVGpickPath*) * ncells);
		
		cell = 1;
		for (l = 1; l < ps->nlevels; l++) {
			ps->levels[l] = &ps->levels[0][cell];
			leveldim = 1 << l;
			cell += leveldim * leveldim;
		}
		
		ps->ncells = ncells;
	}
	memset(ps->levels[0], 0, ps->ncells * sizeof(NVGpickPath*));

	// Allocate temporary storage for nvgHitTestAll results if required.
	if (ps->picked == NULL) {
		ps->cpicked = 16;
		ps->picked = malloc(sizeof(NVGpickPath*) * ps->cpicked);
	}
	
	ps->npoints = 0;
	ps->nsegments = 0;
}

