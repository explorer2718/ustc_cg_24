#include "light.h"

#include "Utils/Logging/Logging.h"
#include "pxr/base/gf/ray.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/imaging/glf/simpleLight.h"
#include "pxr/imaging/hd/changeTracker.h"
#include "pxr/imaging/hd/rprimCollection.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "utils/math.hpp"
#include "utils/sampling.hpp"

USTC_CG_NAMESPACE_OPEN_SCOPE
using namespace pxr;
void Hd_USTC_CG_Light::Sync(
    HdSceneDelegate* sceneDelegate,
    HdRenderParam* renderParam,
    HdDirtyBits* dirtyBits)
{
    TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    TF_UNUSED(renderParam);

    if (!TF_VERIFY(sceneDelegate != nullptr)) {
        return;
    }

    const SdfPath& id = GetId();

    // HdStLight communicates to the scene graph and caches all interesting
    // values within this class. Later on Get() is called from
    // TaskState (RenderPass) to perform aggregation/pre-computation,
    // in order to make the shader execution efficient.

    // Change tracking
    HdDirtyBits bits = *dirtyBits;

    // Transform
    if (bits & DirtyTransform) {
        _params[HdTokens->transform] = VtValue(sceneDelegate->GetTransform(id));
    }

    std::ostringstream val;
    val << VtValue(sceneDelegate->GetTransform(id)).Cast<GfMatrix4d>();

    USTC_CG::logging(val.str(), USTC_CG::Info);

    // Lighting Params
    if (bits & DirtyParams) {
        HdChangeTracker& changeTracker = sceneDelegate->GetRenderIndex().GetChangeTracker();

        // Remove old dependencies
        VtValue val = Get(HdTokens->filters);
        if (val.IsHolding<SdfPathVector>()) {
            auto lightFilterPaths = val.UncheckedGet<SdfPathVector>();
            for (const SdfPath& filterPath : lightFilterPaths) {
                changeTracker.RemoveSprimSprimDependency(filterPath, id);
            }
        }

        if (_lightType == HdPrimTypeTokens->simpleLight) {
            _params[HdLightTokens->params] = sceneDelegate->Get(id, HdLightTokens->params);
        }
        // else if (_lightType == HdPrimTypeTokens->domeLight)
        //{
        //     _params[HdLightTokens->params] =
        //         _PrepareDomeLight(id, sceneDelegate);
        // }
        //// If it is an area light we will extract the parameters and convert
        //// them to a GlfSimpleLight that approximates the light source.
        // else
        //{
        //     _params[HdLightTokens->params] =
        //         _ApproximateAreaLight(id, sceneDelegate);
        // }

        // Add new dependencies
        val = Get(HdTokens->filters);
        if (val.IsHolding<SdfPathVector>()) {
            auto lightFilterPaths = val.UncheckedGet<SdfPathVector>();
            for (const SdfPath& filterPath : lightFilterPaths) {
                changeTracker.AddSprimSprimDependency(filterPath, id);
            }
        }
    }

    if (bits & (DirtyTransform | DirtyParams)) {
        auto transform = Get(HdTokens->transform).GetWithDefault<GfMatrix4d>();
        // Update cached light objects.  Note that simpleLight ignores
        // scene-delegate transform, in favor of the transform passed in by
        // params...
        if (_lightType == HdPrimTypeTokens->domeLight) {
            // Apply domeOffset if present
            VtValue domeOffset = sceneDelegate->GetLightParamValue(id, HdLightTokens->domeOffset);
            if (domeOffset.IsHolding<GfMatrix4d>()) {
                transform = domeOffset.UncheckedGet<GfMatrix4d>() * transform;
            }
            auto light = Get(HdLightTokens->params).GetWithDefault<GlfSimpleLight>();
            light.SetTransform(transform);
            _params[HdLightTokens->params] = VtValue(light);
        }
        else if (_lightType != HdPrimTypeTokens->simpleLight) {
            // e.g. area light
            auto light = Get(HdLightTokens->params).GetWithDefault<GlfSimpleLight>();
            GfVec3d p = transform.ExtractTranslation();
            GfVec4f pos(p[0], p[1], p[2], 1.0f);
            // Convention is to emit light along -Z
            GfVec4d zDir = transform.GetRow(2);
            if (_lightType == HdPrimTypeTokens->rectLight ||
                _lightType == HdPrimTypeTokens->diskLight) {
                light.SetSpotDirection(GfVec3f(-zDir[0], -zDir[1], -zDir[2]));
            }
            else if (_lightType == HdPrimTypeTokens->distantLight) {
                // For a distant light, translate to +Z homogeneous limit
                // See simpleLighting.glslfx : integrateLightsDefault.
                pos = GfVec4f(zDir[0], zDir[1], zDir[2], 0.0f);
            }
            else if (_lightType == HdPrimTypeTokens->sphereLight) {
                _params[HdLightTokens->radius] =
                    sceneDelegate->GetLightParamValue(id, HdLightTokens->radius);
            }
            auto diffuse =
                sceneDelegate->GetLightParamValue(id, HdLightTokens->diffuse).Get<float>();
            auto color =
                sceneDelegate->GetLightParamValue(id, HdLightTokens->color).Get<GfVec3f>() *
                diffuse;
            light.SetDiffuse(GfVec4f(color[0], color[1], color[2], 0));
            light.SetPosition(pos);
            _params[HdLightTokens->params] = VtValue(light);
        }
    }

    // Shadow Params
    if (bits & DirtyShadowParams) {
        _params[HdLightTokens->shadowParams] =
            sceneDelegate->GetLightParamValue(id, HdLightTokens->shadowParams);
    }

    // Shadow Collection
    if (bits & DirtyCollection) {
        VtValue vtShadowCollection =
            sceneDelegate->GetLightParamValue(id, HdLightTokens->shadowCollection);

        // Optional
        if (vtShadowCollection.IsHolding<HdRprimCollection>()) {
            auto newCollection = vtShadowCollection.UncheckedGet<HdRprimCollection>();

            if (_params[HdLightTokens->shadowCollection] != newCollection) {
                _params[HdLightTokens->shadowCollection] = VtValue(newCollection);

                HdChangeTracker& changeTracker = sceneDelegate->GetRenderIndex().GetChangeTracker();

                changeTracker.MarkCollectionDirty(newCollection.GetName());
            }
        }
        else {
            _params[HdLightTokens->shadowCollection] = VtValue(HdRprimCollection());
        }
    }

    *dirtyBits = Clean;
}

HdDirtyBits Hd_USTC_CG_Light::GetInitialDirtyBitsMask() const
{
    if (_lightType == HdPrimTypeTokens->simpleLight ||
        _lightType == HdPrimTypeTokens->distantLight) {
        return AllDirty;
    }
    else {
        return (DirtyParams | DirtyTransform);
    }
}

Color Hd_USTC_CG_Light::Sample(
    const GfVec3f& pos,
    GfVec3f& dir,
    float& sample_light_pdf,
    const std::function<float()>& uniform_float)
{
    if (_lightType == HdPrimTypeTokens->sphereLight) {
        auto simplelight = Get(HdLightTokens->params).Get<GlfSimpleLight>();
        auto radius = Get(HdLightTokens->radius).Get<float>();

        auto lightPos = simplelight.GetPosition();
        auto lightPos3 = GfVec3f(lightPos[0], lightPos[1], lightPos[2]);

        auto distanceVec = lightPos3 - pos;

        auto basis = constructONB(-distanceVec.GetNormalized());

        auto distance = distanceVec.GetLength();

        // A sphere light is treated as all points on the surface spreads energy uniformly:

        float sample_pos_pdf;
        // First we sample a point on the hemi sphere:
        auto sampledDir =
            UniformSampleHemiSphere(GfVec2f(uniform_float(), uniform_float()), sample_pos_pdf);
        auto worldSampledDir = basis * sampledDir;

        auto sampledPosOnSurface = worldSampledDir * radius + lightPos3;

        // Then we can decide the direction.
        dir = (sampledPosOnSurface - pos).GetNormalized();

        // and the pdf (with the measure of solid angle):
        float cosVal = GfDot(-dir, worldSampledDir.GetNormalized());

        float area = 4 * M_PI * radius * radius;

        sample_light_pdf = sample_pos_pdf / radius / radius * cosVal / distance / distance;

        // Finally we calculate the radiance
        auto powerIntotal4 = simplelight.GetDiffuse();
        auto powerIntotal = GfVec3f(powerIntotal4[0], powerIntotal4[1], powerIntotal4[2]);

        auto irradiance = powerIntotal / area;
        if (cosVal < 0) {
            return Color{ 0 };
        }
        return irradiance * cosVal / distance / distance / M_PI;
    }
    else {
        return { 1, 0., 1 };
    }
}

Color Hd_USTC_CG_Light::Intersect(const GfRay& ray, float& depth)
{
    if (_lightType == HdPrimTypeTokens->sphereLight) {
        auto simplelight = Get(HdLightTokens->params).Get<GlfSimpleLight>();
        auto radius = Get(HdLightTokens->radius).Get<float>();

        auto lightPos = simplelight.GetPosition();
        auto lightPos3 = GfVec3d(lightPos[0], lightPos[1], lightPos[2]);
        double enterdistance;
        ray.Intersect(lightPos3, radius, &enterdistance);
        depth = enterdistance;
        return true;
    }
}

VtValue Hd_USTC_CG_Light::Get(const TfToken& token) const
{
    VtValue val;
    TfMapLookup(_params, token, &val);
    return val;
}

USTC_CG_NAMESPACE_CLOSE_SCOPE
