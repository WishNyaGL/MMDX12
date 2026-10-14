////////////////////////////////////////////////////////////////////////////////////////////////
// MMDX12 built-in "standard" MME effect (M5a, docs/mme/mme_design.md 2.8).
//
// Clean-room reproduction of MMD's default look written in MME's own format. It is used for
//   * DefaultEffect `none` / a model with no matching DefaultEffect line (offscreen draws),
//   * a model whose effect has no matching technique,
//   * the built-in zplot pass of the self-shadow map.
// It goes through the same M1 parser / M2 compiler, which makes it the runtime's regression test.
//
// Techniques: object / object_ss (self-shadow on or off), edge, zplot, shadow.
// Colour model (MME semantics; the MMD ambient/emissive quirk lives in the binder):
//   ambient = saturate(MaterialAmbient * LightAmbient + MaterialEmissive)
//   + diffuse only without toon, * texture (+adding/multiplying), sphere add/mul,
//   toon tex2D(toon, float2(0, 0.5 - 0.5*N.L)) (object_ss also * the self-shadow term),
//   + specular, alpha = diffuse.a * texture.a.
////////////////////////////////////////////////////////////////////////////////////////////////

// --- matrices / scene -------------------------------------------------------
float4x4 WorldViewProjMatrix      : WORLDVIEWPROJECTION;
float4x4 WorldMatrix              : WORLD;
float4x4 ViewMatrix               : VIEW;
float4x4 LightWorldViewProjMatrix : WORLDVIEWPROJECTION < string Object = "Light"; >;

float3   LightDirection : DIRECTION < string Object = "Light"; >;
float3   CameraPosition : POSITION  < string Object = "Camera"; >;

// --- material (Geometry) ----------------------------------------------------
float4   MaterialDiffuse  : DIFFUSE  < string Object = "Geometry"; >;
float3   MaterialAmbient  : AMBIENT  < string Object = "Geometry"; >;
float3   MaterialEmmisive : EMISSIVE < string Object = "Geometry"; >;
float3   MaterialSpecular : SPECULAR < string Object = "Geometry"; >;
float    SpecularPower    : SPECULARPOWER < string Object = "Geometry"; >;
float3   MaterialToon     : TOONCOLOR;
float4   EdgeColor        : EDGECOLOR;

// --- light ------------------------------------------------------------------
float3   LightDiffuse  : DIFFUSE  < string Object = "Light"; >;
float3   LightAmbient  : AMBIENT  < string Object = "Light"; >;
float3   LightSpecular : SPECULAR < string Object = "Light"; >;

static float4 DiffuseColor  = MaterialDiffuse  * float4(LightDiffuse, 1.0f);
static float3 AmbientColor  = saturate(MaterialAmbient * LightAmbient + MaterialEmmisive);
static float3 SpecularColor = MaterialSpecular * LightSpecular;
static float3 EdgeColorRGB  = EdgeColor.rgb;

// --- by-name uniforms (spec draft 5): material flags, no semantic -----------
bool use_texture;
bool use_spheremap;
bool use_toon;
bool spadd;
bool parthf;
bool transp;
bool opadd;

// --- textures ---------------------------------------------------------------
texture ObjectTexture    : MATERIALTEXTURE;
sampler ObjTexSampler = sampler_state {
    texture = <ObjectTexture>;
    MINFILTER = LINEAR; MAGFILTER = LINEAR; MIPFILTER = LINEAR;
    ADDRESSU = WRAP; ADDRESSV = WRAP;
};

texture ObjectSphereMap  : MATERIALSPHEREMAP;
sampler ObjSphereSampler = sampler_state {
    texture = <ObjectSphereMap>;
    MINFILTER = LINEAR; MAGFILTER = LINEAR; MIPFILTER = LINEAR;
    ADDRESSU = WRAP; ADDRESSV = WRAP;
};

texture ObjectToonMap    : MATERIALTOONTEXTURE;
sampler ObjToonSampler = sampler_state {
    texture = <ObjectToonMap>;
    MINFILTER = LINEAR; MAGFILTER = LINEAR; MIPFILTER = LINEAR;
    ADDRESSU = CLAMP; ADDRESSV = CLAMP;
};

// MMD's self-shadow map: bound at register(s0), no `texture =` (MMD fills the register).
sampler SelfShadowMap : register(s0);

////////////////////////////////////////////////////////////////////////////////////////////////
// object / object_ss

struct VS_OUTPUT {
    float4 Pos     : POSITION;
    float2 Tex     : TEXCOORD0;
    float3 Normal  : TEXCOORD1;
    float2 SpTex   : TEXCOORD2;
    float3 World   : TEXCOORD3;
};

VS_OUTPUT Object_VS(float4 Pos : POSITION, float3 Normal : NORMAL, float2 Tex : TEXCOORD0) {
    VS_OUTPUT Out = (VS_OUTPUT)0;
    Out.Pos = mul(Pos, WorldViewProjMatrix);
    Out.World = mul(Pos, WorldMatrix).xyz;
    Out.Normal = normalize(mul(Normal, (float3x3)WorldMatrix));
    Out.Tex = Tex;
    float2 nv = mul(Out.Normal, (float3x3)ViewMatrix);
    Out.SpTex.x = nv.x * 0.5f + 0.5f;
    Out.SpTex.y = nv.y * -0.5f + 0.5f;
    return Out;
}

float3 ShadeBody(VS_OUTPUT IN, bool selfShadow) {
    float3 N = normalize(IN.Normal);
    float3 L = -LightDirection;

    float4 Color;
    Color.rgb = AmbientColor;
    Color.a = DiffuseColor.a;
    if (!use_toon) {
        Color.rgb += max(0, dot(N, L)) * DiffuseColor.rgb;
    }
    Color = saturate(Color);

    if (use_texture) {
        Color *= tex2D(ObjTexSampler, IN.Tex);
    }
    if (use_spheremap) {
        float4 s = tex2D(ObjSphereSampler, IN.SpTex);
        if (spadd) Color.rgb += s.rgb;
        else       Color.rgb *= s.rgb;
    }

    float3 toon = float3(1, 1, 1);
    if (use_toon) {
        float ndl = dot(N, L);
        toon = tex2D(ObjToonSampler, float2(0.0f, 0.5f - 0.5f * ndl)).rgb;
    }
    if (selfShadow && parthf) {
        float4 lp = mul(float4(IN.World, 1.0f), LightWorldViewProjMatrix);
        float2 uv = lp.xy / lp.w * float2(0.5f, -0.5f) + 0.5f;
        float depth = lp.z / lp.w;
        float map = tex2D(SelfShadowMap, uv).r;
        float sh = (map < depth - 0.0008f) ? 0.5f : 1.0f;
        toon *= sh;
    }
    Color.rgb *= toon;

    float3 eye = normalize(CameraPosition - IN.World);
    float3 h = normalize(L + eye);
    Color.rgb += pow(max(0, dot(h, N)), max(SpecularPower, 1.0f)) * SpecularColor;

    return Color.rgb;
}

float4 Object_PS(VS_OUTPUT IN) : COLOR0 {
    float4 c;
    c.rgb = ShadeBody(IN, false);
    c.a = AmbientColor.r * 0.0f + DiffuseColor.a;
    return c;
}

float4 ObjectSS_PS(VS_OUTPUT IN) : COLOR0 {
    float4 c;
    c.rgb = ShadeBody(IN, true);
    c.a = DiffuseColor.a;
    return c;
}

////////////////////////////////////////////////////////////////////////////////////////////////
// edge

float4 Edge_VS(float4 Pos : POSITION) : POSITION {
    return mul(Pos, WorldViewProjMatrix);
}

float4 Edge_PS() : COLOR0 {
    return float4(EdgeColorRGB, EdgeColor.a);
}

////////////////////////////////////////////////////////////////////////////////////////////////
// zplot (self-shadow light-space depth)

struct ZPLOT_OUT {
    float4 Pos   : POSITION;
    float  Depth : TEXCOORD0;
};

ZPLOT_OUT ZPlot_VS(float4 Pos : POSITION) {
    ZPLOT_OUT Out = (ZPLOT_OUT)0;
    float4 p = mul(Pos, LightWorldViewProjMatrix);
    Out.Pos = p;
    Out.Depth = p.z / p.w;
    return Out;
}

float4 ZPlot_PS(ZPLOT_OUT IN) : COLOR0 {
    return float4(IN.Depth, 0, 0, 1);
}

////////////////////////////////////////////////////////////////////////////////////////////////
// shadow (ground shadow) -- minimal; MMD uses it for the floor shadow only.

float4 Shadow_VS(float4 Pos : POSITION) : POSITION {
    return mul(Pos, WorldViewProjMatrix);
}

float4 Shadow_PS() : COLOR0 {
    return float4(AmbientColor, 0.65f);
}

////////////////////////////////////////////////////////////////////////////////////////////////
// techniques

technique ObjectTec < string MMDPass = "object"; > {
    pass Object {
        AlphaBlendEnable = TRUE;
        SRCBLEND = SRCALPHA;
        DESTBLEND = INVSRCALPHA;
        VertexShader = compile vs_3_0 Object_VS();
        PixelShader  = compile ps_3_0 Object_PS();
    }
}

technique ObjectSSTec < string MMDPass = "object_ss"; > {
    pass ObjectSS {
        AlphaBlendEnable = TRUE;
        SRCBLEND = SRCALPHA;
        DESTBLEND = INVSRCALPHA;
        VertexShader = compile vs_3_0 Object_VS();
        PixelShader  = compile ps_3_0 ObjectSS_PS();
    }
}

technique EdgeTec < string MMDPass = "edge"; > {
    pass Edge {
        AlphaBlendEnable = FALSE;
        AlphaTestEnable  = FALSE;
        VertexShader = compile vs_3_0 Edge_VS();
        PixelShader  = compile ps_3_0 Edge_PS();
    }
}

technique ZPlotTec < string MMDPass = "zplot"; > {
    pass ZPlot {
        AlphaBlendEnable = FALSE;
        AlphaTestEnable  = FALSE;
        ZWriteEnable = TRUE;
        VertexShader = compile vs_3_0 ZPlot_VS();
        PixelShader  = compile ps_3_0 ZPlot_PS();
    }
}

technique ShadowTec < string MMDPass = "shadow"; > {
    pass Shadow {
        AlphaBlendEnable = TRUE;
        SRCBLEND = SRCALPHA;
        DESTBLEND = INVSRCALPHA;
        VertexShader = compile vs_3_0 Shadow_VS();
        PixelShader  = compile ps_3_0 Shadow_PS();
    }
}
