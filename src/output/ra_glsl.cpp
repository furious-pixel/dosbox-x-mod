/*
 * RetroArch GLSL (.glslp) subset used by the starred CRT presets.
 * Runs on the native VGA blit only. Not used by the enhanced compositor.
 */

#include "dosbox.h"

#if C_OPENGL

#include "ra_glsl.h"

#include "cross.h"
#include "logging.h"
#include "sdlmain.h"

#ifdef WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "SDL_opengl.h"

#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef APIENTRYP
#define APIENTRYP APIENTRY *
#endif

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif
#ifndef GL_SRGB8_ALPHA8
#define GL_SRGB8_ALPHA8 0x8C43
#endif
#ifndef GL_FRAMEBUFFER_SRGB
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_CLAMP_TO_BORDER
#define GL_CLAMP_TO_BORDER 0x812D
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_MIRRORED_REPEAT
#define GL_MIRRORED_REPEAT 0x8370
#endif

#include <png.h>

extern std::string GetDOSBoxXPath(bool withexe = false);

namespace {

typedef void (APIENTRYP RA_PFNGLACTIVETEXTUREPROC)(GLenum);
typedef void (APIENTRYP RA_PFNGLATTACHSHADERPROC)(GLuint, GLuint);
typedef void (APIENTRYP RA_PFNGLBINDBUFFERPROC)(GLenum, GLuint);
typedef void (APIENTRYP RA_PFNGLBINDFRAMEBUFFERPROC)(GLenum, GLuint);
typedef void (APIENTRYP RA_PFNGLCOMPILESHADERPROC)(GLuint);
typedef GLuint (APIENTRYP RA_PFNGLCREATEPROGRAMPROC)(void);
typedef GLuint (APIENTRYP RA_PFNGLCREATESHADERPROC)(GLenum);
typedef void (APIENTRYP RA_PFNGLDELETEFRAMEBUFFERSPROC)(GLsizei, const GLuint *);
typedef void (APIENTRYP RA_PFNGLDELETEPROGRAMPROC)(GLuint);
typedef void (APIENTRYP RA_PFNGLDELETESHADERPROC)(GLuint);
typedef void (APIENTRYP RA_PFNGLDISABLEVERTEXATTRIBARRAYPROC)(GLuint);
typedef void (APIENTRYP RA_PFNGLENABLEVERTEXATTRIBARRAYPROC)(GLuint);
typedef void (APIENTRYP RA_PFNGLFRAMEBUFFERTEXTURE2DPROC)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void (APIENTRYP RA_PFNGLGENFRAMEBUFFERSPROC)(GLsizei, GLuint *);
typedef void (APIENTRYP RA_PFNGLGENERATEMIPMAPPROC)(GLenum);
typedef GLint (APIENTRYP RA_PFNGLGETATTRIBLOCATIONPROC)(GLuint, const GLchar *);
typedef void (APIENTRYP RA_PFNGLGETPROGRAMIVPROC)(GLuint, GLenum, GLint *);
typedef void (APIENTRYP RA_PFNGLGETPROGRAMINFOLOGPROC)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef void (APIENTRYP RA_PFNGLGETSHADERIVPROC)(GLuint, GLenum, GLint *);
typedef void (APIENTRYP RA_PFNGLGETSHADERINFOLOGPROC)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef GLint (APIENTRYP RA_PFNGLGETUNIFORMLOCATIONPROC)(GLuint, const GLchar *);
typedef GLenum (APIENTRYP RA_PFNGLCHECKFRAMEBUFFERSTATUSPROC)(GLenum);
typedef void (APIENTRYP RA_PFNGLLINKPROGRAMPROC)(GLuint);
typedef void (APIENTRYP RA_PFNGLSHADERSOURCEPROC)(GLuint, GLsizei, const GLchar **, const GLint *);
typedef void (APIENTRYP RA_PFNGLUNIFORM1FPROC)(GLint, GLfloat);
typedef void (APIENTRYP RA_PFNGLUNIFORM1IPROC)(GLint, GLint);
typedef void (APIENTRYP RA_PFNGLUNIFORM2FPROC)(GLint, GLfloat, GLfloat);
typedef void (APIENTRYP RA_PFNGLUNIFORMMATRIX4FVPROC)(GLint, GLsizei, GLboolean, const GLfloat *);
typedef void (APIENTRYP RA_PFNGLUSEPROGRAMPROC)(GLuint);
typedef void (APIENTRYP RA_PFNGLVERTEXATTRIBPOINTERPROC)(GLuint, GLint, GLenum, GLboolean, GLsizei, const GLvoid *);
typedef void (APIENTRYP RA_PFNGLBINDVERTEXARRAYPROC)(GLuint);

RA_PFNGLACTIVETEXTUREPROC ra_glActiveTexture = NULL;
RA_PFNGLATTACHSHADERPROC ra_glAttachShader = NULL;
RA_PFNGLBINDBUFFERPROC ra_glBindBuffer = NULL;
RA_PFNGLBINDFRAMEBUFFERPROC ra_glBindFramebuffer = NULL;
RA_PFNGLCOMPILESHADERPROC ra_glCompileShader = NULL;
RA_PFNGLCREATEPROGRAMPROC ra_glCreateProgram = NULL;
RA_PFNGLCREATESHADERPROC ra_glCreateShader = NULL;
RA_PFNGLDELETEFRAMEBUFFERSPROC ra_glDeleteFramebuffers = NULL;
RA_PFNGLDELETEPROGRAMPROC ra_glDeleteProgram = NULL;
RA_PFNGLDELETESHADERPROC ra_glDeleteShader = NULL;
RA_PFNGLDISABLEVERTEXATTRIBARRAYPROC ra_glDisableVertexAttribArray = NULL;
RA_PFNGLENABLEVERTEXATTRIBARRAYPROC ra_glEnableVertexAttribArray = NULL;
RA_PFNGLFRAMEBUFFERTEXTURE2DPROC ra_glFramebufferTexture2D = NULL;
RA_PFNGLGENFRAMEBUFFERSPROC ra_glGenFramebuffers = NULL;
RA_PFNGLGENERATEMIPMAPPROC ra_glGenerateMipmap = NULL;
RA_PFNGLGETATTRIBLOCATIONPROC ra_glGetAttribLocation = NULL;
RA_PFNGLGETPROGRAMIVPROC ra_glGetProgramiv = NULL;
RA_PFNGLGETPROGRAMINFOLOGPROC ra_glGetProgramInfoLog = NULL;
RA_PFNGLGETSHADERIVPROC ra_glGetShaderiv = NULL;
RA_PFNGLGETSHADERINFOLOGPROC ra_glGetShaderInfoLog = NULL;
RA_PFNGLGETUNIFORMLOCATIONPROC ra_glGetUniformLocation = NULL;
RA_PFNGLCHECKFRAMEBUFFERSTATUSPROC ra_glCheckFramebufferStatus = NULL;
RA_PFNGLLINKPROGRAMPROC ra_glLinkProgram = NULL;
RA_PFNGLSHADERSOURCEPROC ra_glShaderSource = NULL;
RA_PFNGLUNIFORM1FPROC ra_glUniform1f = NULL;
RA_PFNGLUNIFORM1IPROC ra_glUniform1i = NULL;
RA_PFNGLUNIFORM2FPROC ra_glUniform2f = NULL;
RA_PFNGLUNIFORMMATRIX4FVPROC ra_glUniformMatrix4fv = NULL;
RA_PFNGLUSEPROGRAMPROC ra_glUseProgram = NULL;
RA_PFNGLVERTEXATTRIBPOINTERPROC ra_glVertexAttribPointer = NULL;
RA_PFNGLBINDVERTEXARRAYPROC ra_glBindVertexArray = NULL;

bool g_procs_loaded = false;

static void *RA_GetProc(const char *name)
{
	void *p = (void *)SDL_GL_GetProcAddress(name);
	if (p)
		return p;
	std::string ext = std::string(name) + "ARB";
	p = (void *)SDL_GL_GetProcAddress(ext.c_str());
	if (p)
		return p;
	ext = std::string(name) + "EXT";
	return (void *)SDL_GL_GetProcAddress(ext.c_str());
}

static bool LoadProcs(void)
{
	if (g_procs_loaded)
		return true;
	ra_glActiveTexture = (RA_PFNGLACTIVETEXTUREPROC)RA_GetProc("glActiveTexture");
	ra_glAttachShader = (RA_PFNGLATTACHSHADERPROC)RA_GetProc("glAttachShader");
	ra_glBindBuffer = (RA_PFNGLBINDBUFFERPROC)RA_GetProc("glBindBuffer");
	ra_glBindFramebuffer = (RA_PFNGLBINDFRAMEBUFFERPROC)RA_GetProc("glBindFramebuffer");
	ra_glCompileShader = (RA_PFNGLCOMPILESHADERPROC)RA_GetProc("glCompileShader");
	ra_glCreateProgram = (RA_PFNGLCREATEPROGRAMPROC)RA_GetProc("glCreateProgram");
	ra_glCreateShader = (RA_PFNGLCREATESHADERPROC)RA_GetProc("glCreateShader");
	ra_glDeleteFramebuffers = (RA_PFNGLDELETEFRAMEBUFFERSPROC)RA_GetProc("glDeleteFramebuffers");
	ra_glDeleteProgram = (RA_PFNGLDELETEPROGRAMPROC)RA_GetProc("glDeleteProgram");
	ra_glDeleteShader = (RA_PFNGLDELETESHADERPROC)RA_GetProc("glDeleteShader");
	ra_glDisableVertexAttribArray = (RA_PFNGLDISABLEVERTEXATTRIBARRAYPROC)RA_GetProc("glDisableVertexAttribArray");
	ra_glEnableVertexAttribArray = (RA_PFNGLENABLEVERTEXATTRIBARRAYPROC)RA_GetProc("glEnableVertexAttribArray");
	ra_glFramebufferTexture2D = (RA_PFNGLFRAMEBUFFERTEXTURE2DPROC)RA_GetProc("glFramebufferTexture2D");
	ra_glGenFramebuffers = (RA_PFNGLGENFRAMEBUFFERSPROC)RA_GetProc("glGenFramebuffers");
	ra_glGenerateMipmap = (RA_PFNGLGENERATEMIPMAPPROC)RA_GetProc("glGenerateMipmap");
	ra_glGetAttribLocation = (RA_PFNGLGETATTRIBLOCATIONPROC)RA_GetProc("glGetAttribLocation");
	ra_glGetProgramiv = (RA_PFNGLGETPROGRAMIVPROC)RA_GetProc("glGetProgramiv");
	ra_glGetProgramInfoLog = (RA_PFNGLGETPROGRAMINFOLOGPROC)RA_GetProc("glGetProgramInfoLog");
	ra_glGetShaderiv = (RA_PFNGLGETSHADERIVPROC)RA_GetProc("glGetShaderiv");
	ra_glGetShaderInfoLog = (RA_PFNGLGETSHADERINFOLOGPROC)RA_GetProc("glGetShaderInfoLog");
	ra_glGetUniformLocation = (RA_PFNGLGETUNIFORMLOCATIONPROC)RA_GetProc("glGetUniformLocation");
	ra_glCheckFramebufferStatus = (RA_PFNGLCHECKFRAMEBUFFERSTATUSPROC)RA_GetProc("glCheckFramebufferStatus");
	ra_glLinkProgram = (RA_PFNGLLINKPROGRAMPROC)RA_GetProc("glLinkProgram");
	ra_glShaderSource = (RA_PFNGLSHADERSOURCEPROC)RA_GetProc("glShaderSource");
	ra_glUniform1f = (RA_PFNGLUNIFORM1FPROC)RA_GetProc("glUniform1f");
	ra_glUniform1i = (RA_PFNGLUNIFORM1IPROC)RA_GetProc("glUniform1i");
	ra_glUniform2f = (RA_PFNGLUNIFORM2FPROC)RA_GetProc("glUniform2f");
	ra_glUniformMatrix4fv = (RA_PFNGLUNIFORMMATRIX4FVPROC)RA_GetProc("glUniformMatrix4fv");
	ra_glUseProgram = (RA_PFNGLUSEPROGRAMPROC)RA_GetProc("glUseProgram");
	ra_glVertexAttribPointer = (RA_PFNGLVERTEXATTRIBPOINTERPROC)RA_GetProc("glVertexAttribPointer");
	ra_glBindVertexArray = (RA_PFNGLBINDVERTEXARRAYPROC)RA_GetProc("glBindVertexArray");

	g_procs_loaded = ra_glActiveTexture && ra_glAttachShader && ra_glBindBuffer &&
		ra_glBindFramebuffer && ra_glCompileShader &&
		ra_glCreateProgram && ra_glCreateShader &&
		ra_glDeleteFramebuffers && ra_glDeleteProgram && ra_glDeleteShader &&
		ra_glEnableVertexAttribArray && ra_glFramebufferTexture2D &&
		ra_glGenFramebuffers && ra_glGetAttribLocation &&
		ra_glGetProgramiv && ra_glGetShaderiv && ra_glGetUniformLocation &&
		ra_glCheckFramebufferStatus && ra_glLinkProgram && ra_glShaderSource &&
		ra_glUniform1f && ra_glUniform1i && ra_glUniform2f &&
		ra_glUniformMatrix4fv && ra_glUseProgram && ra_glVertexAttribPointer;
	if (!g_procs_loaded)
		LOG_MSG("RA_GLSL: missing required OpenGL entry points");
	return g_procs_loaded;
}

static std::string Trim(const std::string &s)
{
	size_t a = 0;
	while (a < s.size() && isspace((unsigned char)s[a]))
		++a;
	size_t b = s.size();
	while (b > a && isspace((unsigned char)s[b - 1]))
		--b;
	return s.substr(a, b - a);
}

static std::string StripComment(const std::string &s)
{
	std::string out;
	char quote = 0;
	for (size_t i = 0; i < s.size(); ++i) {
		char c = s[i];
		if (quote) {
			out.push_back(c);
			if (c == quote && (i == 0 || s[i - 1] != '\\'))
				quote = 0;
			continue;
		}
		if (c == '"' || c == '\'') {
			quote = c;
			out.push_back(c);
			continue;
		}
		if (c == '#' || (c == '/' && i + 1 < s.size() && s[i + 1] == '/'))
			break;
		out.push_back(c);
	}
	return Trim(out);
}

static std::string StripQuotes(std::string v)
{
	v = StripComment(v);
	if (v.size() >= 2 && ((v[0] == '"' && v[v.size() - 1] == '"') ||
			      (v[0] == '\'' && v[v.size() - 1] == '\'')))
		return v.substr(1, v.size() - 2);
	if (!v.empty() && v[v.size() - 1] == ';')
		v.resize(v.size() - 1);
	return Trim(v);
}

static bool ParseBool(const std::string &v)
{
	std::string s = StripQuotes(v);
	for (size_t i = 0; i < s.size(); ++i)
		s[i] = (char)tolower((unsigned char)s[i]);
	return s == "true" || s == "1" || s == "yes" || s == "on";
}

static bool ParseFloat(const std::string &v, float *out)
{
	std::string s = StripQuotes(v);
	if (s.empty())
		return false;
	char *end = NULL;
	float f = (float)strtod(s.c_str(), &end);
	if (end == s.c_str())
		return false;
	*out = f;
	return true;
}

static bool FileExists(const std::string &path)
{
	std::ifstream f(path.c_str(), std::ios::binary);
	return f.good();
}

static std::string ParentDir(const std::string &path)
{
	size_t slash = path.find_last_of("/\\");
	if (slash == std::string::npos)
		return std::string(".");
	if (slash == 0)
		return path.substr(0, 1);
	return path.substr(0, slash);
}

static std::string JoinPath(const std::string &base, const std::string &rel)
{
	if (rel.empty())
		return base;
	if (rel.size() >= 2 && rel[1] == ':')
		return rel;
	if (rel[0] == '/' || rel[0] == '\\')
		return rel;
	if (base.empty() || base == ".")
		return rel;
	char sep = '/';
#ifdef WIN32
	sep = '\\';
#endif
	if (base[base.size() - 1] == '/' || base[base.size() - 1] == '\\')
		return base + rel;
	return base + sep + rel;
}

static std::string LowerCopy(std::string s)
{
	for (size_t i = 0; i < s.size(); ++i) {
		if (s[i] == '\\')
			s[i] = '/';
		else
			s[i] = (char)tolower((unsigned char)s[i]);
	}
	return s;
}

static bool ReadTextFile(const std::string &path, std::string &out)
{
	std::ifstream f(path.c_str(), std::ios::binary);
	if (!f.good())
		return false;
	std::ostringstream ss;
	ss << f.rdbuf();
	out = ss.str();
	return true;
}

struct RaPassSpec {
	std::string shader;
	bool filter_linear;
	bool float_framebuffer;
	bool srgb_framebuffer;
	bool mipmap_input;
	std::string scale_type;
	std::string scale_type_x;
	std::string scale_type_y;
	bool has_scale;
	bool has_scale_x;
	bool has_scale_y;
	float scale;
	float scale_x;
	float scale_y;
	std::string wrap_mode;
	std::string alias;
	RaPassSpec()
	    : filter_linear(false), float_framebuffer(false), srgb_framebuffer(false),
	      mipmap_input(false), has_scale(false), has_scale_x(false), has_scale_y(false),
	      scale(1.0f), scale_x(1.0f), scale_y(1.0f)
	{
	}
};

struct RaLutSpec {
	std::string name;
	std::string path;
	bool linear;
	std::string wrap_mode;
	bool mipmap;
	RaLutSpec() : linear(true), wrap_mode("clamp_to_border"), mipmap(false) {}
};

struct RaPreset {
	std::string path;
	std::vector<RaPassSpec> passes;
	std::map<std::string, float> parameters;
	std::vector<RaLutSpec> luts;
};

static bool IsMetaKey(const std::string &k, const std::vector<std::string> &lut_names)
{
	if (k == "shaders" || k == "parameters" || k == "textures" || k == "feedback_pass")
		return true;
	for (size_t i = 0; i < lut_names.size(); ++i) {
		if (k == lut_names[i])
			return true;
	}
	if (k.compare(0, 6, "shader") == 0 || k.compare(0, 7, "filter_") == 0 ||
	    k.compare(0, 5, "scale") == 0 || k.compare(0, 6, "float_") == 0 ||
	    k.compare(0, 5, "srgb_") == 0 || k.compare(0, 5, "wrap_") == 0 ||
	    k.compare(0, 5, "alias") == 0 || k.compare(0, 7, "mipmap_") == 0)
		return true;
	if (k.size() >= 7 && k.compare(k.size() - 7, 7, "_linear") == 0)
		return true;
	if (k.size() >= 10 && k.compare(k.size() - 10, 10, "_wrap_mode") == 0)
		return true;
	if (k.size() >= 7 && k.compare(k.size() - 7, 7, "_mipmap") == 0)
		return true;
	return false;
}

static bool ParseGlslp(const std::string &path, RaPreset &preset, std::string &err)
{
	std::string text;
	if (!ReadTextFile(path, text)) {
		err = "cannot read " + path;
		return false;
	}
	std::map<std::string, std::string> kv;
	std::istringstream in(text);
	std::string raw;
	while (std::getline(in, raw)) {
		std::string line = Trim(raw);
		if (line.empty() || line[0] == '#' || (line.size() >= 2 && line[0] == '/' && line[1] == '/'))
			continue;
		size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		kv[Trim(line.substr(0, eq))] = line.substr(eq + 1);
	}

	int n = 1;
	if (kv.count("shaders")) {
		float nf = 1.0f;
		if (ParseFloat(kv["shaders"], &nf))
			n = (int)nf;
	}
	if (n < 1 || n > 32) {
		err = path + ": invalid shaders count";
		return false;
	}

	const std::string base = ParentDir(path);
	preset.path = path;
	preset.passes.clear();
	preset.parameters.clear();
	preset.luts.clear();

	for (int i = 0; i < n; ++i) {
		char key[64];
		sprintf(key, "shader%d", i);
		if (!kv.count(key)) {
			err = path + ": missing " + key;
			return false;
		}
		RaPassSpec p;
		p.shader = JoinPath(base, StripQuotes(kv[key]));
		if (!FileExists(p.shader)) {
			err = path + ": shader not found: " + p.shader;
			return false;
		}
		sprintf(key, "filter_linear%d", i);
		if (kv.count(key))
			p.filter_linear = ParseBool(kv[key]);
		sprintf(key, "float_framebuffer%d", i);
		if (kv.count(key))
			p.float_framebuffer = ParseBool(kv[key]);
		sprintf(key, "srgb_framebuffer%d", i);
		if (kv.count(key))
			p.srgb_framebuffer = ParseBool(kv[key]);
		sprintf(key, "mipmap_input%d", i);
		if (kv.count(key))
			p.mipmap_input = ParseBool(kv[key]);
		sprintf(key, "scale_type%d", i);
		if (kv.count(key))
			p.scale_type = StripQuotes(kv[key]);
		sprintf(key, "scale_type_x%d", i);
		if (kv.count(key))
			p.scale_type_x = StripQuotes(kv[key]);
		sprintf(key, "scale_type_y%d", i);
		if (kv.count(key))
			p.scale_type_y = StripQuotes(kv[key]);
		sprintf(key, "scale%d", i);
		if (kv.count(key) && ParseFloat(kv[key], &p.scale))
			p.has_scale = true;
		sprintf(key, "scale_x%d", i);
		if (kv.count(key) && ParseFloat(kv[key], &p.scale_x))
			p.has_scale_x = true;
		sprintf(key, "scale_y%d", i);
		if (kv.count(key) && ParseFloat(kv[key], &p.scale_y))
			p.has_scale_y = true;
		sprintf(key, "wrap_mode%d", i);
		if (kv.count(key))
			p.wrap_mode = StripQuotes(kv[key]);
		else {
			sprintf(key, "texture_wrap_mode%d", i);
			if (kv.count(key))
				p.wrap_mode = StripQuotes(kv[key]);
		}
		sprintf(key, "alias%d", i);
		if (kv.count(key))
			p.alias = StripQuotes(kv[key]);
		preset.passes.push_back(p);
	}

	std::vector<std::string> lut_names;
	if (kv.count("textures")) {
		std::string listed = StripQuotes(kv["textures"]);
		std::istringstream ts(listed);
		std::string name;
		while (std::getline(ts, name, ';')) {
			name = Trim(name);
			if (!name.empty())
				lut_names.push_back(name);
		}
	}
	for (size_t i = 0; i < lut_names.size(); ++i) {
		const std::string &name = lut_names[i];
		if (!kv.count(name))
			continue;
		RaLutSpec lut;
		lut.name = name;
		lut.path = JoinPath(base, StripQuotes(kv[name]));
		if (kv.count(name + "_linear"))
			lut.linear = ParseBool(kv[name + "_linear"]);
		else if (kv.count(name + "linear"))
			lut.linear = ParseBool(kv[name + "linear"]);
		if (kv.count(name + "_wrap_mode"))
			lut.wrap_mode = StripQuotes(kv[name + "_wrap_mode"]);
		if (kv.count(name + "_mipmap"))
			lut.mipmap = ParseBool(kv[name + "_mipmap"]);
		preset.luts.push_back(lut);
	}

	std::vector<std::string> listed_params;
	if (kv.count("parameters")) {
		std::istringstream ps(StripQuotes(kv["parameters"]));
		std::string name;
		while (std::getline(ps, name, ';')) {
			name = Trim(name);
			if (!name.empty())
				listed_params.push_back(name);
		}
	}
	for (std::map<std::string, std::string>::const_iterator it = kv.begin(); it != kv.end(); ++it) {
		bool listed = false;
		for (size_t i = 0; i < listed_params.size(); ++i) {
			if (listed_params[i] == it->first) {
				listed = true;
				break;
			}
		}
		if (!listed) {
			if (it->first.empty() || !isalpha((unsigned char)it->first[0]))
				continue;
			if (IsMetaKey(it->first, lut_names))
				continue;
			bool ident = true;
			for (size_t i = 0; i < it->first.size(); ++i) {
				char c = it->first[i];
				if (!(isalnum((unsigned char)c) || c == '_')) {
					ident = false;
					break;
				}
			}
			if (!ident)
				continue;
		}
		float f = 0.0f;
		if (ParseFloat(it->second, &f))
			preset.parameters[it->first] = f;
	}
	return true;
}

static std::string ResolveIncludes(const std::string &source, const std::string &base_dir, int depth)
{
	if (depth > 16)
		return source;
	std::string out;
	std::istringstream in(source);
	std::string line;
	while (std::getline(in, line)) {
		std::string t = Trim(line);
		if (t.compare(0, 8, "#include") == 0) {
			size_t q1 = t.find_first_of("\"<");
			size_t q2 = t.find_last_of("\">");
			if (q1 != std::string::npos && q2 > q1) {
				std::string rel = t.substr(q1 + 1, q2 - q1 - 1);
				std::string cand = JoinPath(base_dir, rel);
				std::string body;
				if (ReadTextFile(cand, body)) {
					out += ResolveIncludes(body, ParentDir(cand), depth + 1);
					out += '\n';
					continue;
				}
			}
		}
		out += line;
		out += '\n';
	}
	return out;
}

static void CollectParameters(const std::string &source, std::map<std::string, float> &out)
{
	const char *p = source.c_str();
	while ((p = strstr(p, "#pragma parameter")) != NULL) {
		p += 18;
		char name[128];
		char label[256];
		float defv = 0.0f, mn = 0.0f, mx = 0.0f, step = 0.0f;
		if (sscanf(p, " %127s \"%255[^\"]\" %f %f %f %f", name, label, &defv, &mn, &mx, &step) >= 3)
			out[name] = defv;
	}
}

static std::string PrepareStage(const std::string &source, bool vertex)
{
	std::string body = source;
	std::string version = "#version 120\n";
	size_t ver = body.find("#version ");
	if (ver != std::string::npos) {
		size_t nl = body.find('\n', ver);
		if (nl == std::string::npos)
			nl = body.size();
		version = body.substr(ver, nl - ver + (nl < body.size() ? 1 : 0));
		body.erase(ver, nl - ver + (nl < body.size() ? 1 : 0));
	}
	std::string stage = vertex ? "VERTEX" : "FRAGMENT";
	return version + "#define " + stage + " 1\n#define PARAMETER_UNIFORM\n" + body;
}

static void LogInfo(const char *what, GLuint obj, bool shader)
{
	GLint len = 0;
	if (shader)
		ra_glGetShaderiv(obj, GL_INFO_LOG_LENGTH, &len);
	else
		ra_glGetProgramiv(obj, GL_INFO_LOG_LENGTH, &len);
	if (len <= 1)
		return;
	std::vector<char> buf((size_t)len + 1);
	if (shader)
		ra_glGetShaderInfoLog(obj, len, NULL, &buf[0]);
	else
		ra_glGetProgramInfoLog(obj, len, NULL, &buf[0]);
	LOG_MSG("RA_GLSL: %s: %s", what, &buf[0]);
}

static GLuint CompileShader(GLenum type, const std::string &src, const std::string &tag)
{
	GLuint sh = ra_glCreateShader(type);
	const char *p = src.c_str();
	GLint len = (GLint)src.size();
	ra_glShaderSource(sh, 1, &p, &len);
	ra_glCompileShader(sh);
	GLint ok = 0;
	ra_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		LogInfo((tag + (type == GL_VERTEX_SHADER ? " vertex" : " fragment")).c_str(), sh, true);
		ra_glDeleteShader(sh);
		return 0;
	}
	return sh;
}

static GLuint LinkProgram(GLuint vs, GLuint fs, const std::string &tag)
{
	GLuint prog = ra_glCreateProgram();
	ra_glAttachShader(prog, vs);
	ra_glAttachShader(prog, fs);
	ra_glLinkProgram(prog);
	ra_glDeleteShader(vs);
	ra_glDeleteShader(fs);
	GLint ok = 0;
	ra_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		LogInfo((tag + " link").c_str(), prog, false);
		ra_glDeleteProgram(prog);
		return 0;
	}
	return prog;
}

static GLenum WrapEnum(const std::string &mode)
{
	std::string m = LowerCopy(mode);
	if (m.find("repeat") != std::string::npos && m.find("mirror") != std::string::npos)
		return GL_MIRRORED_REPEAT;
	if (m.find("repeat") != std::string::npos)
		return GL_REPEAT;
	if (m.find("border") != std::string::npos)
		return GL_CLAMP_TO_BORDER;
	return GL_CLAMP_TO_EDGE;
}

static void ApplyWrap(GLenum wrap)
{
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
	if (wrap == GL_CLAMP_TO_BORDER) {
		const GLfloat border[4] = {0.0f, 0.0f, 0.0f, 0.0f};
		glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
	}
}

static bool LoadPngRgba(const std::string &path, std::vector<unsigned char> &rgba, int *w, int *h, bool flip)
{
	FILE *fp = fopen(path.c_str(), "rb");
	if (!fp)
		return false;
	png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (!png) {
		fclose(fp);
		return false;
	}
	png_infop info = png_create_info_struct(png);
	if (!info) {
		png_destroy_read_struct(&png, NULL, NULL);
		fclose(fp);
		return false;
	}
	if (setjmp(png_jmpbuf(png))) {
		png_destroy_read_struct(&png, &info, NULL);
		fclose(fp);
		return false;
	}
	png_init_io(png, fp);
	png_read_info(png, info);
	*w = (int)png_get_image_width(png, info);
	*h = (int)png_get_image_height(png, info);
	png_byte color = png_get_color_type(png, info);
	png_byte depth = png_get_bit_depth(png, info);
	if (depth == 16)
		png_set_strip_16(png);
	if (color == PNG_COLOR_TYPE_PALETTE)
		png_set_palette_to_rgb(png);
	if (color == PNG_COLOR_TYPE_GRAY && depth < 8)
		png_set_expand_gray_1_2_4_to_8(png);
	if (png_get_valid(png, info, PNG_INFO_tRNS))
		png_set_tRNS_to_alpha(png);
	if (color == PNG_COLOR_TYPE_RGB || color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_PALETTE)
		png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
	if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA)
		png_set_gray_to_rgb(png);
	png_read_update_info(png, info);
	rgba.resize((size_t)(*w) * (size_t)(*h) * 4u);
	std::vector<png_bytep> rows((size_t)*h);
	for (int y = 0; y < *h; ++y) {
		int row = flip ? (*h - 1 - y) : y;
		rows[(size_t)y] = &rgba[(size_t)row * (size_t)(*w) * 4u];
	}
	png_read_image(png, &rows[0]);
	png_destroy_read_struct(&png, &info, NULL);
	fclose(fp);
	return true;
}

struct CompiledPass {
	RaPassSpec spec;
	GLuint program;
	GLint loc_vertex;
	GLint loc_texcoord;
	GLint loc_color;
	CompiledPass() : program(0), loc_vertex(-1), loc_texcoord(-1), loc_color(-1) {}
};

struct Target {
	GLuint tex;
	GLuint fbo;
	int w;
	int h;
	bool use_float;
	bool use_srgb;
	Target() : tex(0), fbo(0), w(0), h(0), use_float(false), use_srgb(false) {}
};

struct LutTex {
	std::string name;
	GLuint tex;
	int w;
	int h;
	bool linear;
	std::string wrap_mode;
	LutTex() : tex(0), w(0), h(0), linear(true) {}
};

struct CopyProg {
	GLuint program;
	GLint loc_pos;
	GLint loc_uvscale;
	CopyProg() : program(0), loc_pos(-1), loc_uvscale(-1) {}
};

static const float kIdentity[16] = {
	1, 0, 0, 0,
	0, 1, 0, 0,
	0, 0, 1, 0,
	0, 0, 0, 1
};

static const float kQuad[4 * 4] = {
	-1.0f, -1.0f, 0.0f, 1.0f,
	 1.0f, -1.0f, 0.0f, 1.0f,
	-1.0f,  1.0f, 0.0f, 1.0f,
	 1.0f,  1.0f, 0.0f, 1.0f
};

static const float kUv[4 * 4] = {
	0.0f, 0.0f, 0.0f, 1.0f,
	1.0f, 0.0f, 0.0f, 1.0f,
	0.0f, 1.0f, 0.0f, 1.0f,
	1.0f, 1.0f, 0.0f, 1.0f
};

static const float kCol[4 * 4] = {
	1, 1, 1, 1,
	1, 1, 1, 1,
	1, 1, 1, 1,
	1, 1, 1, 1
};

struct Chain {
	std::string path;
	RaPreset preset;
	std::vector<CompiledPass> passes;
	std::vector<Target> targets;
	std::vector<LutTex> luts;
	std::map<std::string, float> params;
	CopyProg copy;
	GLuint src_tex;
	GLuint src_fbo;
	int src_w;
	int src_h;
	unsigned long long generation;
	bool compiled;
	Chain() : src_tex(0), src_fbo(0), src_w(0), src_h(0), generation(0), compiled(false) {}
};

static std::string g_preset_path;
static Chain g_chain;

static void DeleteTarget(Target &t)
{
	if (t.fbo) {
		ra_glDeleteFramebuffers(1, &t.fbo);
		t.fbo = 0;
	}
	if (t.tex) {
		glDeleteTextures(1, &t.tex);
		t.tex = 0;
	}
	t.w = t.h = 0;
}

static void ReleaseChainGL(Chain &c)
{
	for (size_t i = 0; i < c.passes.size(); ++i) {
		if (c.passes[i].program) {
			ra_glDeleteProgram(c.passes[i].program);
			c.passes[i].program = 0;
		}
	}
	c.passes.clear();
	for (size_t i = 0; i < c.targets.size(); ++i)
		DeleteTarget(c.targets[i]);
	c.targets.clear();
	for (size_t i = 0; i < c.luts.size(); ++i) {
		if (c.luts[i].tex)
			glDeleteTextures(1, &c.luts[i].tex);
	}
	c.luts.clear();
	if (c.copy.program) {
		ra_glDeleteProgram(c.copy.program);
		c.copy.program = 0;
	}
	if (c.src_fbo) {
		ra_glDeleteFramebuffers(1, &c.src_fbo);
		c.src_fbo = 0;
	}
	if (c.src_tex) {
		glDeleteTextures(1, &c.src_tex);
		c.src_tex = 0;
	}
	c.src_w = c.src_h = 0;
	c.compiled = false;
}

static bool MakeTarget(Target &t, int w, int h, bool use_float, bool use_srgb)
{
	if (t.tex && t.w == w && t.h == h && t.use_float == use_float && t.use_srgb == use_srgb)
		return true;
	DeleteTarget(t);
	t.w = w;
	t.h = h;
	t.use_float = use_float;
	t.use_srgb = use_srgb && !use_float;
	glGenTextures(1, &t.tex);
	glBindTexture(GL_TEXTURE_2D, t.tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	ApplyWrap(GL_CLAMP_TO_EDGE);
	GLenum internal = GL_RGBA8;
	GLenum type = GL_UNSIGNED_BYTE;
	if (t.use_float) {
		internal = GL_RGBA16F;
		type = GL_FLOAT;
	} else if (t.use_srgb) {
		internal = GL_SRGB8_ALPHA8;
	}
	glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal, w, h, 0, GL_RGBA, type, NULL);
	ra_glGenFramebuffers(1, &t.fbo);
	ra_glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
	ra_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
	GLenum status = ra_glCheckFramebufferStatus(GL_FRAMEBUFFER);
	ra_glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		LOG_MSG("RA_GLSL: FBO incomplete (%u) %dx%d float=%d srgb=%d",
			(unsigned)status, w, h, (int)t.use_float, (int)t.use_srgb);
		DeleteTarget(t);
		return false;
	}
	return true;
}

static bool EnsureSrcTex(Chain &c, int w, int h)
{
	if (c.src_tex && c.src_fbo && c.src_w == w && c.src_h == h)
		return true;
	if (c.src_fbo) {
		ra_glDeleteFramebuffers(1, &c.src_fbo);
		c.src_fbo = 0;
	}
	if (c.src_tex)
		glDeleteTextures(1, &c.src_tex);
	c.src_tex = 0;
	glGenTextures(1, &c.src_tex);
	glBindTexture(GL_TEXTURE_2D, c.src_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	ApplyWrap(GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	ra_glGenFramebuffers(1, &c.src_fbo);
	ra_glBindFramebuffer(GL_FRAMEBUFFER, c.src_fbo);
	ra_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c.src_tex, 0);
	const GLenum status = ra_glCheckFramebufferStatus(GL_FRAMEBUFFER);
	ra_glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	c.src_w = w;
	c.src_h = h;
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		LOG_MSG("RA_GLSL: source FBO incomplete (%u)", (unsigned)status);
		return false;
	}
	return c.src_tex != 0 && c.src_fbo != 0;
}

static bool CompileCopy(Chain &c)
{
	if (c.copy.program)
		return true;
	static const char *vs =
		"#version 120\n"
		"attribute vec4 a_pos;\n"
		"varying vec2 v_uv;\n"
		"uniform vec2 uvScale;\n"
		"void main(){\n"
		"  gl_Position = a_pos;\n"
		"  vec2 uv = a_pos.xy * 0.5 + 0.5;\n"
		"  v_uv = vec2(uv.x * uvScale.x, (1.0 - uv.y) * uvScale.y);\n"
		"}\n";
	static const char *fs =
		"#version 120\n"
		"varying vec2 v_uv;\n"
		"uniform sampler2D Texture;\n"
		"void main(){ gl_FragColor = texture2D(Texture, v_uv); }\n";
	GLuint vsh = CompileShader(GL_VERTEX_SHADER, vs, "copy");
	GLuint fsh = CompileShader(GL_FRAGMENT_SHADER, fs, "copy");
	if (!vsh || !fsh)
		return false;
	c.copy.program = LinkProgram(vsh, fsh, "copy");
	if (!c.copy.program)
		return false;
	c.copy.loc_pos = ra_glGetAttribLocation(c.copy.program, "a_pos");
	c.copy.loc_uvscale = ra_glGetUniformLocation(c.copy.program, "uvScale");
	return true;
}

static void ApplyShellSafe(Chain &c)
{
	const std::string path = LowerCopy(c.preset.path);
	const bool is_guest = path.find("guest-dr-venom") != std::string::npos ||
			      (path.find("guest") != std::string::npos && path.find("venom") != std::string::npos);
	if (!is_guest)
		return;
	c.params["interm"] = 0.0f;
	if (!c.params.count("inter") || c.params["inter"] < 600.0f)
		c.params["inter"] = 800.0f;
	if (c.params.count("warpX"))
		c.params["warpX"] = 0.0f;
	if (c.params.count("warpY"))
		c.params["warpY"] = 0.0f;
}

static bool LoadLuts(Chain &c)
{
	for (size_t i = 0; i < c.preset.luts.size(); ++i) {
		const RaLutSpec &spec = c.preset.luts[i];
		if (!FileExists(spec.path)) {
			LOG_MSG("RA_GLSL: missing LUT %s: %s", spec.name.c_str(), spec.path.c_str());
			continue;
		}
		const std::string name_l = LowerCopy(spec.name);
		const std::string path_l = LowerCopy(spec.path);
		const bool is_color_lut = name_l.find("samplerlut") == 0 ||
					  name_l.find("lut") != std::string::npos ||
					  path_l.find("/lut/") != std::string::npos;
		std::vector<unsigned char> rgba;
		int w = 0, h = 0;
		if (!LoadPngRgba(spec.path, rgba, &w, &h, !is_color_lut)) {
			LOG_MSG("RA_GLSL: failed to read LUT %s", spec.path.c_str());
			continue;
		}
		LutTex lut;
		lut.name = spec.name;
		lut.w = w;
		lut.h = h;
		lut.linear = spec.linear;
		lut.wrap_mode = spec.wrap_mode;
		glGenTextures(1, &lut.tex);
		glBindTexture(GL_TEXTURE_2D, lut.tex);
		GLint filter = spec.linear ? GL_LINEAR : GL_NEAREST;
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
		ApplyWrap(WrapEnum(spec.wrap_mode));
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, &rgba[0]);
		glBindTexture(GL_TEXTURE_2D, 0);
		c.luts.push_back(lut);
	}
	return true;
}

static bool CompileChain(Chain &c)
{
	ReleaseChainGL(c);
	if (!LoadProcs())
		return false;
	if (!CompileCopy(c))
		return false;

	std::map<std::string, float> defaults;
	for (size_t i = 0; i < c.preset.passes.size(); ++i) {
		const RaPassSpec &spec = c.preset.passes[i];
		std::string raw;
		if (!ReadTextFile(spec.shader, raw)) {
			LOG_MSG("RA_GLSL: cannot read %s", spec.shader.c_str());
			return false;
		}
		raw = ResolveIncludes(raw, ParentDir(spec.shader), 0);
		CollectParameters(raw, defaults);
		std::string vs = PrepareStage(raw, true);
		std::string fs = PrepareStage(raw, false);
		GLuint vsh = CompileShader(GL_VERTEX_SHADER, vs, spec.shader);
		if (!vsh)
			return false;
		GLuint fsh = CompileShader(GL_FRAGMENT_SHADER, fs, spec.shader);
		if (!fsh) {
			ra_glDeleteShader(vsh);
			return false;
		}
		CompiledPass cp;
		cp.spec = spec;
		cp.program = LinkProgram(vsh, fsh, spec.shader);
		if (!cp.program)
			return false;
		cp.loc_vertex = ra_glGetAttribLocation(cp.program, "VertexCoord");
		cp.loc_texcoord = ra_glGetAttribLocation(cp.program, "TexCoord");
		cp.loc_color = ra_glGetAttribLocation(cp.program, "COLOR");
		if (cp.loc_color < 0)
			cp.loc_color = ra_glGetAttribLocation(cp.program, "Color");
		c.passes.push_back(cp);
	}
	c.params = defaults;
	for (std::map<std::string, float>::const_iterator it = c.preset.parameters.begin();
	     it != c.preset.parameters.end(); ++it)
		c.params[it->first] = it->second;
	ApplyShellSafe(c);
	if (!LoadLuts(c))
		return false;
	c.targets.resize(c.passes.size());
	c.compiled = true;
	LOG_MSG("RA_GLSL: compiled %s (%u passes)", c.path.c_str(), (unsigned)c.passes.size());
	return true;
}

static int AxisSize(const std::string &scale_type, bool has_scale, float scale,
		    int prev, int viewport, bool is_last)
{
	const float sc = has_scale ? scale : 1.0f;
	std::string st = LowerCopy(scale_type);
	if (st.empty()) {
		if (is_last)
			return std::max(1, (int)floor((double)viewport * sc + 0.5));
		return std::max(1, (int)floor((double)prev * sc + 0.5));
	}
	if (st == "viewport")
		return std::max(1, (int)floor((double)viewport * sc + 0.5));
	if (st == "absolute")
		return std::max(1, (int)floor((double)sc + 0.5));
	return std::max(1, (int)floor((double)prev * sc + 0.5));
}

static void PassOutSize(const RaPassSpec &p, int prev_w, int prev_h, int vw, int vh,
			bool is_last, int *out_w, int *out_h)
{
	const std::string stx = p.scale_type_x.empty() ? p.scale_type : p.scale_type_x;
	const std::string sty = p.scale_type_y.empty() ? p.scale_type : p.scale_type_y;
	const bool has_sx = p.has_scale_x || p.has_scale;
	const bool has_sy = p.has_scale_y || p.has_scale;
	const float sx = p.has_scale_x ? p.scale_x : p.scale;
	const float sy = p.has_scale_y ? p.scale_y : p.scale;
	*out_w = AxisSize(stx, has_sx, sx, prev_w, vw, is_last);
	*out_h = AxisSize(sty, has_sy, sy, prev_h, vh, is_last);
}

static void BindTextureUnit(GLint loc, int unit, GLuint tex, bool linear, bool mipmap,
			    const std::string &wrap)
{
	if (loc < 0)
		return;
	ra_glActiveTexture(GL_TEXTURE0 + unit);
	glBindTexture(GL_TEXTURE_2D, tex);
	GLint mag = linear ? GL_LINEAR : GL_NEAREST;
	GLint minf = mag;
	if (mipmap && ra_glGenerateMipmap) {
		ra_glGenerateMipmap(GL_TEXTURE_2D);
		minf = linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST;
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minf);
	ApplyWrap(WrapEnum(wrap));
	ra_glUniform1i(loc, unit);
}

static GLint U(GLuint prog, const char *name)
{
	return ra_glGetUniformLocation(prog, name);
}

static void SetVec2(GLuint prog, const char *name, float x, float y)
{
	GLint loc = U(prog, name);
	if (loc >= 0)
		ra_glUniform2f(loc, x, y);
}

static void SetCommon(const CompiledPass &cp, int tex_w, int tex_h, int out_w, int out_h, int frame)
{
	const GLuint prog = cp.program;
	GLint mvp = U(prog, "MVPMatrix");
	if (mvp >= 0)
		ra_glUniformMatrix4fv(mvp, 1, GL_FALSE, kIdentity);
	GLint fd = U(prog, "FrameDirection");
	if (fd >= 0)
		ra_glUniform1i(fd, 1);
	GLint fc = U(prog, "FrameCount");
	if (fc >= 0)
		ra_glUniform1i(fc, frame);
	SetVec2(prog, "OutputSize", (float)out_w, (float)out_h);
	SetVec2(prog, "TextureSize", (float)tex_w, (float)tex_h);
	SetVec2(prog, "InputSize", (float)tex_w, (float)tex_h);
	for (std::map<std::string, float>::const_iterator it = g_chain.params.begin();
	     it != g_chain.params.end(); ++it) {
		GLint loc = U(prog, it->first.c_str());
		if (loc >= 0)
			ra_glUniform1f(loc, it->second);
	}
}

struct HistEntry {
	GLuint tex;
	int w;
	int h;
	HistEntry() : tex(0), w(0), h(0) {}
	HistEntry(GLuint t, int ww, int hh) : tex(t), w(ww), h(hh) {}
};

static int BindHistory(GLuint prog, const std::vector<HistEntry> &history,
		       const std::map<std::string, HistEntry> &aliases, int unit)
{
	if (!history.empty()) {
		static const char *orig_names[] = {"Original", "OrigTexture", NULL};
		for (int i = 0; orig_names[i]; ++i) {
			GLint loc = U(prog, orig_names[i]);
			if (loc >= 0) {
				BindTextureUnit(loc, unit++, history[0].tex, false, false, "clamp_to_edge");
			}
		}
		SetVec2(prog, "OriginalSize", (float)history[0].w, (float)history[0].h);
		SetVec2(prog, "OrigTextureSize", (float)history[0].w, (float)history[0].h);
		SetVec2(prog, "OrigInputSize", (float)history[0].w, (float)history[0].h);
		SetVec2(prog, "OriginalTextureSize", (float)history[0].w, (float)history[0].h);
		SetVec2(prog, "OriginalInputSize", (float)history[0].w, (float)history[0].h);
	}

	for (int n = 1; n <= 15; ++n) {
		char name[64];
		sprintf(name, "PassPrev%dTexture", n);
		GLint loc = U(prog, name);
		if (loc < 0)
			continue;
		int idx = (int)history.size() - n;
		if (idx < 0)
			idx = 0;
		const HistEntry &e = history[(size_t)idx];
		BindTextureUnit(loc, unit++, e.tex, false, false, "clamp_to_edge");
		char s1[64], s2[64], s3[64];
		sprintf(s1, "PassPrev%dTextureSize", n);
		sprintf(s2, "PassPrev%dInputSize", n);
		sprintf(s3, "PassPrev%dSize", n);
		SetVec2(prog, s1, (float)e.w, (float)e.h);
		SetVec2(prog, s2, (float)e.w, (float)e.h);
		SetVec2(prog, s3, (float)e.w, (float)e.h);
	}

	for (int n = 0; n < 16; ++n) {
		char name[64];
		sprintf(name, "PassOutput%d", n);
		GLint loc = U(prog, name);
		int hist = n + 1;
		if (loc >= 0 && hist < (int)history.size()) {
			const HistEntry &e = history[(size_t)hist];
			BindTextureUnit(loc, unit++, e.tex, false, false, "clamp_to_edge");
			char s1[64];
			sprintf(s1, "PassOutput%dSize", n);
			SetVec2(prog, s1, (float)e.w, (float)e.h);
		}
	}
	for (int n = 1; n < 16; ++n) {
		char name[64];
		sprintf(name, "Pass%dTexture", n);
		GLint loc = U(prog, name);
		if (loc >= 0 && n < (int)history.size()) {
			const HistEntry &e = history[(size_t)n];
			BindTextureUnit(loc, unit++, e.tex, false, false, "clamp_to_edge");
			char s1[64], s2[64];
			sprintf(s1, "Pass%dTextureSize", n);
			sprintf(s2, "Pass%dInputSize", n);
			SetVec2(prog, s1, (float)e.w, (float)e.h);
			SetVec2(prog, s2, (float)e.w, (float)e.h);
		}
	}

	for (std::map<std::string, HistEntry>::const_iterator it = aliases.begin(); it != aliases.end(); ++it) {
		const std::string names[4] = {it->first, it->first + "Texture", it->first + "texture", it->first + "Pass"};
		for (int i = 0; i < 4; ++i) {
			if (names[i] == "Texture" || names[i] == "Source" || names[i] == "s_p" || names[i] == "Original")
				continue;
			GLint loc = U(prog, names[i].c_str());
			if (loc >= 0)
				BindTextureUnit(loc, unit++, it->second.tex, false, false, "clamp_to_edge");
		}
		SetVec2(prog, (it->first + "Size").c_str(), (float)it->second.w, (float)it->second.h);
		SetVec2(prog, (it->first + "TextureSize").c_str(), (float)it->second.w, (float)it->second.h);
		SetVec2(prog, (it->first + "InputSize").c_str(), (float)it->second.w, (float)it->second.h);
	}

	/* Guest afterglow / avg-lum declare Prev* frame-history samplers. Bind
	 * the current original so they stay defined; trails simply do not accumulate. */
	if (!history.empty()) {
		static const char *prev_names[] = {
			"PrevTexture", "Prev1Texture", "Prev2Texture", "Prev3Texture",
			"Prev4Texture", "Prev5Texture", "Prev6Texture", NULL
		};
		for (int i = 0; prev_names[i]; ++i) {
			GLint loc = U(prog, prev_names[i]);
			if (loc >= 0)
				BindTextureUnit(loc, unit++, history[0].tex, false, false, "clamp_to_edge");
		}
	}
	return unit;
}

static int BindLuts(GLuint prog, int unit)
{
	for (size_t i = 0; i < g_chain.luts.size(); ++i) {
		const LutTex &lut = g_chain.luts[i];
		const std::string names[3] = {lut.name, lut.name + "Texture", lut.name + "texture"};
		bool bound = false;
		for (int n = 0; n < 3; ++n) {
			GLint loc = U(prog, names[n].c_str());
			if (loc >= 0) {
				BindTextureUnit(loc, unit++, lut.tex, lut.linear, false, lut.wrap_mode);
				bound = true;
			}
		}
		if (bound) {
			SetVec2(prog, (lut.name + "Size").c_str(), (float)lut.w, (float)lut.h);
			SetVec2(prog, (lut.name + "TextureSize").c_str(), (float)lut.w, (float)lut.h);
			SetVec2(prog, (lut.name + "texture_size").c_str(), (float)lut.w, (float)lut.h);
		}
	}
	return unit;
}

static void EnableAttribs(const CompiledPass &cp)
{
	if (cp.loc_vertex >= 0) {
		ra_glEnableVertexAttribArray((GLuint)cp.loc_vertex);
		ra_glVertexAttribPointer((GLuint)cp.loc_vertex, 4, GL_FLOAT, GL_FALSE, 0, kQuad);
	}
	if (cp.loc_texcoord >= 0) {
		ra_glEnableVertexAttribArray((GLuint)cp.loc_texcoord);
		ra_glVertexAttribPointer((GLuint)cp.loc_texcoord, 4, GL_FLOAT, GL_FALSE, 0, kUv);
	}
	if (cp.loc_color >= 0) {
		ra_glEnableVertexAttribArray((GLuint)cp.loc_color);
		ra_glVertexAttribPointer((GLuint)cp.loc_color, 4, GL_FLOAT, GL_FALSE, 0, kCol);
	}
}

static void DisableAttribs(const CompiledPass &cp)
{
	if (!ra_glDisableVertexAttribArray)
		return;
	if (cp.loc_vertex >= 0)
		ra_glDisableVertexAttribArray((GLuint)cp.loc_vertex);
	if (cp.loc_texcoord >= 0)
		ra_glDisableVertexAttribArray((GLuint)cp.loc_texcoord);
	if (cp.loc_color >= 0)
		ra_glDisableVertexAttribArray((GLuint)cp.loc_color);
}

static void RestoreAfterDraw(GLuint src_texture)
{
	if (ra_glBindFramebuffer)
		ra_glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDisable(GL_FRAMEBUFFER_SRGB);
	if (ra_glUseProgram)
		ra_glUseProgram(0);
	if (ra_glActiveTexture) {
		for (int u = 7; u >= 0; --u) {
			ra_glActiveTexture(GL_TEXTURE0 + u);
			glBindTexture(GL_TEXTURE_2D, 0);
		}
		ra_glActiveTexture(GL_TEXTURE0);
	}
	glBindTexture(GL_TEXTURE_2D, src_texture);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

static void BindCompatDrawState(void)
{
	if (ra_glBindVertexArray)
		ra_glBindVertexArray(0);
	if (ra_glBindBuffer)
		ra_glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static bool CopySource(Chain &c, GLuint pot_tex, int tex_size, int src_w, int src_h)
{
	if (!EnsureSrcTex(c, src_w, src_h) || !c.copy.program)
		return false;
	BindCompatDrawState();
	ra_glBindFramebuffer(GL_FRAMEBUFFER, c.src_fbo);
	glViewport(0, 0, src_w, src_h);
	glDisable(GL_FRAMEBUFFER_SRGB);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	ra_glUseProgram(c.copy.program);
	if (c.copy.loc_uvscale >= 0) {
		const float sx = (tex_size > 0) ? ((float)src_w / (float)tex_size) : 1.0f;
		const float sy = (tex_size > 0) ? ((float)src_h / (float)tex_size) : 1.0f;
		ra_glUniform2f(c.copy.loc_uvscale, sx, sy);
	}
	GLint loc_tex = U(c.copy.program, "Texture");
	if (loc_tex >= 0)
		ra_glUniform1i(loc_tex, 0);
	ra_glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, pot_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	if (c.copy.loc_pos >= 0) {
		ra_glEnableVertexAttribArray((GLuint)c.copy.loc_pos);
		ra_glVertexAttribPointer((GLuint)c.copy.loc_pos, 4, GL_FLOAT, GL_FALSE, 0, kQuad);
	}
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	if (c.copy.loc_pos >= 0 && ra_glDisableVertexAttribArray)
		ra_glDisableVertexAttribArray((GLuint)c.copy.loc_pos);
	return true;
}

static bool RunChain(GLuint pot_tex, int tex_size, int src_w, int src_h,
		     int vx, int vy, int vw, int vh, int frame)
{
	Chain &c = g_chain;
	BindCompatDrawState();
	if (!CopySource(c, pot_tex, tex_size, src_w, src_h))
		return false;

	std::vector<HistEntry> history;
	history.push_back(HistEntry(c.src_tex, src_w, src_h));
	std::map<std::string, HistEntry> aliases;
	aliases["Original"] = history[0];

	const int n = (int)c.passes.size();
	for (int i = 0; i < n; ++i) {
		const CompiledPass &cp = c.passes[(size_t)i];
		const HistEntry &prev = history.back();
		const bool is_last = (i == n - 1);
		int out_w = 0, out_h = 0;
		PassOutSize(cp.spec, prev.w, prev.h, vw, vh, is_last, &out_w, &out_h);
		const bool use_float = cp.spec.float_framebuffer;
		const bool use_srgb = cp.spec.srgb_framebuffer && !use_float;
		const bool direct = is_last && out_w == vw && out_h == vh;

		if (direct) {
			ra_glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glViewport(vx, vy, vw, vh);
		} else {
			if (!MakeTarget(c.targets[(size_t)i], out_w, out_h, use_float, use_srgb))
				return false;
			ra_glBindFramebuffer(GL_FRAMEBUFFER, c.targets[(size_t)i].fbo);
			glViewport(0, 0, out_w, out_h);
		}

		if (use_srgb && !direct)
			glEnable(GL_FRAMEBUFFER_SRGB);
		else
			glDisable(GL_FRAMEBUFFER_SRGB);

		if (!direct) {
			glDisable(GL_SCISSOR_TEST);
			glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
			glClear(GL_COLOR_BUFFER_BIT);
		}

		ra_glUseProgram(cp.program);
		static const char *src_names[] = {"Texture", "Source", "s_p", "tex", "TEX", NULL};
		int unit = 0;
		for (int s = 0; src_names[s]; ++s) {
			GLint loc = U(cp.program, src_names[s]);
			if (loc >= 0) {
				BindTextureUnit(loc, unit, prev.tex, cp.spec.filter_linear,
						cp.spec.mipmap_input, cp.spec.wrap_mode);
				if (unit == 0)
					unit = 1;
			}
		}
		if (unit == 0) {
			BindTextureUnit(U(cp.program, "Texture"), 0, prev.tex, cp.spec.filter_linear,
					cp.spec.mipmap_input, cp.spec.wrap_mode);
			unit = 1;
		}
		SetCommon(cp, prev.w, prev.h, out_w, out_h, frame);
		unit = BindHistory(cp.program, history, aliases, unit);
		BindLuts(cp.program, unit);
		EnableAttribs(cp);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		DisableAttribs(cp);

		if (use_srgb)
			glDisable(GL_FRAMEBUFFER_SRGB);

		if (direct)
			return true;

		const HistEntry produced(c.targets[(size_t)i].tex, out_w, out_h);
		history.push_back(produced);
		if (!cp.spec.alias.empty())
			aliases[cp.spec.alias] = produced;
	}
	return true;
}

static bool TryPresetFile(const std::string &path, std::string &out)
{
	if (path.empty())
		return false;
	if (FileExists(path)) {
		out = path;
		return true;
	}
	if (FileExists(path + ".glslp")) {
		out = path + ".glslp";
		return true;
	}
	return false;
}

static std::string NormalizePresetId(std::string name)
{
	for (size_t i = 0; i < name.size(); ++i) {
		if (name[i] == '\\')
			name[i] = '/';
	}
	if (name.size() >= 6) {
		std::string ext = name.substr(name.size() - 6);
		for (size_t i = 0; i < ext.size(); ++i)
			ext[i] = (char)tolower((unsigned char)ext[i]);
		if (ext == ".glslp")
			name.resize(name.size() - 6);
	}
	while (!name.empty() && (name[0] == '/' || name[0] == '.')) {
		if (name.compare(0, 2, "./") == 0)
			name.erase(0, 2);
		else if (name[0] == '/')
			name.erase(0, 1);
		else
			break;
	}
	return name;
}

static bool EndsWithGlslp(const char *name)
{
	const size_t n = strlen(name);
	if (n < 6)
		return false;
	return tolower((unsigned char)name[n - 6]) == '.' &&
	       tolower((unsigned char)name[n - 5]) == 'g' &&
	       tolower((unsigned char)name[n - 4]) == 'l' &&
	       tolower((unsigned char)name[n - 3]) == 's' &&
	       tolower((unsigned char)name[n - 2]) == 'l' &&
	       tolower((unsigned char)name[n - 1]) == 'p';
}

static void CollectGlslp(const std::string &abs_dir, const std::string &rel,
			 std::vector<std::string> &out)
{
#ifdef WIN32
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA((abs_dir + "\\*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE)
		return;
	do {
		if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
			continue;
		const std::string child_rel = rel.empty() ? fd.cFileName : (rel + "/" + fd.cFileName);
		const std::string child_abs = abs_dir + "\\" + fd.cFileName;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			CollectGlslp(child_abs, child_rel, out);
		else if (EndsWithGlslp(fd.cFileName))
			out.push_back(NormalizePresetId(child_rel));
	} while (FindNextFileA(h, &fd));
	FindClose(h);
#else
	DIR *dir = opendir(abs_dir.c_str());
	if (!dir)
		return;
	struct dirent *ent;
	while ((ent = readdir(dir)) != NULL) {
		if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
			continue;
		const std::string child_rel = rel.empty() ? ent->d_name : (rel + "/" + ent->d_name);
		const std::string child_abs = abs_dir + "/" + ent->d_name;
		struct stat st;
		if (stat(child_abs.c_str(), &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode))
			CollectGlslp(child_abs, child_rel, out);
		else if (EndsWithGlslp(ent->d_name))
			out.push_back(NormalizePresetId(child_rel));
	}
	closedir(dir);
#endif
}

static bool ListInstalledPresets(std::vector<std::string> &out)
{
	out.clear();
	const std::string exe = GetDOSBoxXPath();
	if (exe.empty())
		return false;
	CollectGlslp(exe + "glshaders", "", out);
	std::sort(out.begin(), out.end());
	return !out.empty();
}

static bool SamePresetId(const std::string &a, const std::string &b)
{
#ifdef WIN32
	return LowerCopy(a) == LowerCopy(b);
#else
	return a == b;
#endif
}

} // namespace

static bool IsGlslpPath(const std::string &path)
{
	return path.size() >= 6 && LowerCopy(path.substr(path.size() - 6)) == ".glslp";
}

bool RA_GLSL_FindPreset(const std::string &name, std::string &out_path)
{
	if (name.empty() || name == "none" || name == "default")
		return false;

	std::string f = name;
	for (size_t i = 0; i < f.size(); ++i) {
		if (f[i] == '/')
			f[i] = CROSS_FILESPLIT;
	}

	std::vector<std::string> candidates;
	candidates.push_back(f);
	candidates.push_back(std::string("glshaders") + CROSS_FILESPLIT + f);
	const std::string exe = GetDOSBoxXPath();
	if (!exe.empty())
		candidates.push_back(exe + "glshaders" + CROSS_FILESPLIT + f);
	candidates.push_back(Cross::GetPlatformConfigDir() + "glshaders" + CROSS_FILESPLIT + f);
	candidates.push_back(Cross::GetPlatformResDir() + "glshaders" + CROSS_FILESPLIT + f);

	for (size_t i = 0; i < candidates.size(); ++i) {
		if (TryPresetFile(candidates[i], out_path) && IsGlslpPath(out_path))
			return true;
	}
	out_path.clear();
	return false;
}

bool RA_GLSL_CyclePreset(const std::string &current, int delta, std::string &out_name)
{
	std::vector<std::string> presets;
	if (!ListInstalledPresets(presets)) {
		LOG_MSG("RA_GLSL: no .glslp presets in glshaders next to the executable");
		return false;
	}

	const std::string cur = NormalizePresetId(current);
	int idx = -1;
	for (size_t i = 0; i < presets.size(); ++i) {
		if (SamePresetId(presets[i], cur)) {
			idx = (int)i;
			break;
		}
	}

	const int n = (int)presets.size();
	const int step = (delta >= 0) ? 1 : -1;
	const int next = (idx < 0) ? ((step > 0) ? 0 : n - 1)
				   : ((idx + step + n) % n);
	out_name = presets[(size_t)next];
	return true;
}

void RA_GLSL_SetPresetPath(const char *path)
{
	const std::string next = path ? path : "";
	if (next == g_preset_path)
		return;
	g_preset_path = next;
	ReleaseChainGL(g_chain);
	g_chain = Chain();
	g_chain.path = next;
}

bool RA_GLSL_HasPreset(void)
{
	return !g_preset_path.empty();
}

void RA_GLSL_Release(void)
{
	ReleaseChainGL(g_chain);
	g_chain.generation = 0;
}

bool RA_GLSL_Draw(unsigned int src_texture,
		  int src_tex_size,
		  int src_w,
		  int src_h,
		  int viewport_x,
		  int viewport_y,
		  int viewport_w,
		  int viewport_h,
		  int frame_count,
		  unsigned long long context_generation)
{
	if (g_preset_path.empty())
		return false;
	if (src_w <= 0 || src_h <= 0 || viewport_w <= 0 || viewport_h <= 0)
		return false;
	if (!LoadProcs())
		return false;

	if (g_chain.generation != context_generation) {
		/* Previous context is already gone; drop ids without deleting. */
		g_chain = Chain();
		g_chain.path = g_preset_path;
		g_chain.generation = context_generation;
	}

	if (!g_chain.compiled) {
		std::string err;
		if (!ParseGlslp(g_preset_path, g_chain.preset, err)) {
			LOG_MSG("RA_GLSL: %s", err.c_str());
			return false;
		}
		g_chain.path = g_preset_path;
		if (!CompileChain(g_chain)) {
			ReleaseChainGL(g_chain);
			return false;
		}
	}

	const bool ok = RunChain(src_texture, src_tex_size, src_w, src_h,
				 viewport_x, viewport_y, viewport_w, viewport_h, frame_count);
	RestoreAfterDraw(src_texture);
	if (!ok)
		LOG_MSG("RA_GLSL: draw failed for %s", g_preset_path.c_str());
	return ok;
}

#endif /* C_OPENGL */
