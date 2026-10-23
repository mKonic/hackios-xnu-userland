/*
 * gfx: textures that are client memory (gfx.h, gfx_share_texture), and the bookkeeping that
 * knows when one is drawn into: which texture is bound where, which texture is the colour of
 * which framebuffer, and so whether a draw, a clear or a blit lands in a shared texture.
 */
#include <stdio.h>
#include <string.h>

#include "gfx_enc.h"

#define API __attribute__((visibility("default")))

#define GL_TEXTURE_2D           0x0DE1
#define GL_TEXTURE0             0x84C0
#define GL_FRAMEBUFFER          0x8D40
#define GL_DRAW_FRAMEBUFFER     0x8CA9
#define GL_READ_FRAMEBUFFER     0x8CA8
#define GL_COLOR_ATTACHMENT0    0x8CE0

static struct gfx_shared *
shared_of_key(struct gfx *c, uint64_t key)
{
	for (int i = 0; i < GFX_MAX_SHARED; i++) {
		if (c->shared[i].res && c->shared[i].key == key) {
			return &c->shared[i];
		}
	}
	return NULL;
}

static struct gfx_shared *
shared_of_texture(struct gfx *c, GLuint texture)
{
	for (int i = 0; texture && i < GFX_MAX_SHARED; i++) {
		if (c->shared[i].res && c->shared[i].texture == texture) {
			return &c->shared[i];
		}
	}
	return NULL;
}

int
gfx_share_texture(struct gfx *c, uint64_t key, void *base, uint32_t width, uint32_t height, uint32_t stride,
    int upload)
{
	GLuint texture = c->bound_2d[c->active_unit];
	struct gfx_shared *s;

	if (c->t.res_create == NULL || texture == 0 || c->err) {
		return 0;
	}
	s = shared_of_key(c, key);
	if (s != NULL && (s->width != width || s->height != height)) {
		gfx_forget_shared(c, key);
		s = NULL;
	}
	if (s == NULL) {
		for (int i = 0; s == NULL && i < GFX_MAX_SHARED; i++) {
			if (c->shared[i].res == 0) {
				s = &c->shared[i];
			}
		}
		if (s == NULL || c->t.res_create(c->t.arg, width, height, &s->res) != 0) {
			if (s) {
				s->res = 0;
			}
			return 0;
		}
		s->key = key;
		s->width = width;
		s->height = height;
		upload = 1;
		if (c->t.res_name != NULL) {
			c->t.res_name(c->t.arg, s->res, key);  /* (a kernel without it says ENOTTY: copied as before) */
		}
	}
	/* a texture is shared with one memory at a time */
	struct gfx_shared *old = shared_of_texture(c, texture);
	if (old != NULL && old != s) {
		old->texture = 0;
	}
	s->texture = texture;
	s->base = base;
	s->stride = stride;
	if (upload) {
		gfx_flush(c);
		if (c->t.res_upload(c->t.arg, s->res, 0, 0, width, height, base, stride) != 0) {
			return 0;
		}
		c->uploads++;
		c->upload_bytes += (uint64_t)width * height * 4;
	}
	gfxe_rcBindTexture(c, s->res);
	return 1;
}

void
gfx_forget_shared(struct gfx *c, uint64_t key)
{
	struct gfx_shared *s = shared_of_key(c, key);
	if (s != NULL) {
		gfx_flush(c);
		c->t.res_unref(c->t.arg, s->res);
		memset(s, 0, sizeof(*s));
	}
}

void
gfx_sync_shared(struct gfx *c)
{
	int any = 0;
	for (int i = 0; i < GFX_MAX_SHARED; i++) {
		any |= c->shared[i].res && c->shared[i].dirty;
	}
	if (!any || c->err) {
		return;
	}
	/* the host must have drawn everything before the colour buffers are read */
	gfxe_glFinishRoundTrip(c);
	for (int i = 0; i < GFX_MAX_SHARED; i++) {
		struct gfx_shared *s = &c->shared[i];
		if (s->res && s->dirty && s->base != NULL) {
			/*
			 * Only what was drawn: the rest of the memory may hold what the host's colour buffer does
			 * not (CoreAnimation redraws what changed and keeps the rest), and it is less to copy.
			 * Memory row y is texture row y (gfx.h), so the rectangle is the same in both.
			 */
			uint32_t x = (uint32_t)s->dx0, y = (uint32_t)s->dy0, w = (uint32_t)(s->dx1 - s->dx0),
			    h = (uint32_t)(s->dy1 - s->dy0);
			s->dirty = 0;
			if (w && h && c->t.res_download(c->t.arg, s->res, x, y, w, h,
			    (uint8_t *)s->base + (size_t)y * s->stride + (size_t)x * 4, s->stride) == 0) {
				c->downloads++;
				c->download_bytes += (uint64_t)w * h * 4;
			}
		}
	}
}

static GLint
clamp(GLint v, GLint lo, GLint hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/*
 * A draw, a clear or a blit is about to land in the bound framebuffer: is its colour shared, and
 * where does it reach? A draw: the viewport (the whole texture before glViewport); a clear: the
 * whole; a blit: its destination. The scissor box, when on, cuts all three.
 */
void
gfx_drawing(struct gfx *c, int kind, GLint x0, GLint y0, GLint x1, GLint y1)
{
	if (c->draw_fbo == 0) {
		return;
	}
	for (int i = 0; i < GFX_MAX_FBOS; i++) {
		if (c->fbo_color[i].fbo == c->draw_fbo) {
			struct gfx_shared *s = shared_of_texture(c, c->fbo_color[i].texture);
			if (s == NULL) {
				return;
			}
			GLint w = (GLint)s->width, h = (GLint)s->height;
			if (kind == GFX_CLEAR || (kind == GFX_DRAW && !c->viewport_set)) {
				x0 = 0, y0 = 0, x1 = w, y1 = h;
			} else if (kind == GFX_DRAW) {
				x0 = c->viewport[0], y0 = c->viewport[1];
				x1 = x0 + c->viewport[2], y1 = y0 + c->viewport[3];
			}
			if (c->scissor_on) {
				x0 = x0 > c->scissor[0] ? x0 : c->scissor[0];
				y0 = y0 > c->scissor[1] ? y0 : c->scissor[1];
				x1 = x1 < c->scissor[0] + c->scissor[2] ? x1 : c->scissor[0] + c->scissor[2];
				y1 = y1 < c->scissor[1] + c->scissor[3] ? y1 : c->scissor[1] + c->scissor[3];
			}
			x0 = clamp(x0, 0, w), x1 = clamp(x1, 0, w), y0 = clamp(y0, 0, h), y1 = clamp(y1, 0, h);
			if (x0 >= x1 || y0 >= y1) {
				return;
			}
			if (!s->dirty) {
				s->dx0 = x0, s->dy0 = y0, s->dx1 = x1, s->dy1 = y1;
			} else {
				s->dx0 = x0 < s->dx0 ? x0 : s->dx0, s->dy0 = y0 < s->dy0 ? y0 : s->dy0;
				s->dx1 = x1 > s->dx1 ? x1 : s->dx1, s->dy1 = y1 > s->dy1 ? y1 : s->dy1;
			}
			s->dirty = 1;
			return;
		}
	}
}

/* ---- the calls that tell -------------------------------------------------------------------- */

#define GL_SCISSOR_TEST         0x0C11

API void
glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	c->viewport[0] = x, c->viewport[1] = y, c->viewport[2] = width, c->viewport[3] = height;
	c->viewport_set = 1;
	gfxe_glViewport(c, x, y, width, height);
}

API void
glScissor(GLint x, GLint y, GLsizei width, GLsizei height)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	c->scissor[0] = x, c->scissor[1] = y, c->scissor[2] = width, c->scissor[3] = height;
	gfxe_glScissor(c, x, y, width, height);
}

API void
glEnable(GLenum cap)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	if (cap == GL_SCISSOR_TEST) {
		c->scissor_on = 1;
	}
	gfxe_glEnable(c, cap);
}

API void
glDisable(GLenum cap)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	if (cap == GL_SCISSOR_TEST) {
		c->scissor_on = 0;
	}
	gfxe_glDisable(c, cap);
}

API void
glActiveTexture(GLenum texture)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	if (texture >= GL_TEXTURE0 && texture < GL_TEXTURE0 + 16) {
		c->active_unit = texture - GL_TEXTURE0;
	}
	gfxe_glActiveTexture(c, texture);
}

API void
glBindTexture(GLenum target, GLuint texture)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	if (target == GL_TEXTURE_2D) {
		c->bound_2d[c->active_unit] = texture;
	}
	gfxe_glBindTexture(c, target, texture);
}

API void
glDeleteTextures(GLsizei n, const GLuint *textures)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	for (GLsizei i = 0; i < n; i++) {
		struct gfx_shared *s = shared_of_texture(c, textures[i]);
		if (s != NULL) {
			s->texture = 0;         /* the colour buffer stays, for the memory's next texture */
		}
		for (int u = 0; u < 16; u++) {
			if (c->bound_2d[u] == textures[i]) {
				c->bound_2d[u] = 0;
			}
		}
	}
	gfxe_glDeleteTextures(c, n, textures);
}

API void
glBindFramebuffer(GLenum target, GLuint framebuffer)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) {
		c->draw_fbo = framebuffer;
	}
	gfxe_glBindFramebuffer(c, target, framebuffer);
}

API void
glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	if (attachment == GL_COLOR_ATTACHMENT0 && target != GL_READ_FRAMEBUFFER && c->draw_fbo) {
		int slot = -1;
		for (int i = 0; i < GFX_MAX_FBOS; i++) {
			if (c->fbo_color[i].fbo == c->draw_fbo || (slot < 0 && c->fbo_color[i].fbo == 0)) {
				slot = i;
				if (c->fbo_color[i].fbo == c->draw_fbo) {
					break;
				}
			}
		}
		if (slot >= 0) {
			c->fbo_color[slot].fbo = c->draw_fbo;
			c->fbo_color[slot].texture = texture;
		}
	}
	gfxe_glFramebufferTexture2D(c, target, attachment, textarget, texture, level);
}

API void
glDeleteFramebuffers(GLsizei n, const GLuint *framebuffers)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	for (GLsizei i = 0; i < n; i++) {
		for (int j = 0; j < GFX_MAX_FBOS; j++) {
			if (framebuffers[i] && c->fbo_color[j].fbo == framebuffers[i]) {
				c->fbo_color[j].fbo = c->fbo_color[j].texture = 0;
			}
		}
		if (c->draw_fbo == framebuffers[i]) {
			c->draw_fbo = 0;
		}
	}
	gfxe_glDeleteFramebuffers(c, n, framebuffers);
}

API void
glClear(GLbitfield mask)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	gfx_drawing(c, GFX_CLEAR, 0, 0, 0, 0);
	gfxe_glClear(c, mask);
}

API void
glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1,
    GLint dstY1, GLbitfield mask, GLenum filter)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	gfx_drawing(c, GFX_BLIT, dstX0 < dstX1 ? dstX0 : dstX1, dstY0 < dstY1 ? dstY0 : dstY1,
	    dstX0 < dstX1 ? dstX1 : dstX0, dstY0 < dstY1 ? dstY1 : dstY0);
	gfxe_glBlitFramebuffer(c, srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
}
