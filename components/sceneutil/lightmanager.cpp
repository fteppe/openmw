#include "lightmanager.hpp"

#include <array>

#include <osg/BufferObject>
#include <osg/BufferIndexBinding>
#include <osg/Endian>
#include <osg/Version>
#include <osg/ValueObject>

#include <osgUtil/CullVisitor>

#include <components/sceneutil/util.hpp>

#include <components/misc/hash.hpp>
#include <components/misc/stringops.hpp>

#include <components/debug/debuglog.hpp>

namespace
{
    bool sortLights(const SceneUtil::LightManager::LightSourceViewBound* left, const SceneUtil::LightManager::LightSourceViewBound* right)
    {
        static auto constexpr illuminationBias = 81.f;
        return left->mViewBound.center().length2() - left->mViewBound.radius2()*illuminationBias < right->mViewBound.center().length2() - right->mViewBound.radius2()*illuminationBias;
    }

    float getLightRadius(const osg::Light* light)
    {
        float value = 0.0;
        light->getUserValue("radius", value);
        return value;
    }

    void setLightRadius(osg::Light* light, float value)
    {
        light->setUserValue("radius", value);
    }

    void configurePosition(osg::Matrixf& mat, const osg::Vec4& pos)
    {
        mat(0, 0) = pos.x();
        mat(0, 1) = pos.y();
        mat(0, 2) = pos.z();
    }

    void configureAmbient(osg::Matrixf& mat, const osg::Vec4& color)
    {
        mat(1, 0) = color.r();
        mat(1, 1) = color.g();
        mat(1, 2) = color.b();
    }

    void configureDiffuse(osg::Matrixf& mat, const osg::Vec4& color)
    {
        mat(2, 0) = color.r();
        mat(2, 1) = color.g();
        mat(2, 2) = color.b();
    }

    void configureSpecular(osg::Matrixf& mat, const osg::Vec4& color)
    {
        mat(3, 0) = color.r();
        mat(3, 1) = color.g();
        mat(3, 2) = color.b();
        mat(3, 3) = color.a();
    }

    void configureAttenuation(osg::Matrixf& mat, float c, float l, float q, float r)
    {
        mat(0, 3) = c;
        mat(1, 3) = l;
        mat(2, 3) = q;
        mat(3, 3) = r;
    }

    bool isReflectionCamera(osg::Camera* camera)
    {
        return (camera->getName() == "ReflectionCamera");
    }
}

namespace SceneUtil
{
    static int sLightId = 0;

    // Handles a GLSL shared layout by using configured offsets and strides to fill a continuous buffer, making the data upload to GPU simpler.
    class LightBuffer : public osg::Referenced
    {
    public:

        enum LayoutOffset
        {
            Diffuse,
            DiffuseSign,
            Ambient,
            Specular,
            Position,
            AttenuationRadius
        };

        LightBuffer(int count)
            : mData(new osg::FloatArray(3*4*count))
            , mEndian(osg::getCpuByteOrder())
            , mCount(count)
            , mCachedSunPos(osg::Vec4())
        {
        }

        LightBuffer(const LightBuffer&) = delete;

        void setDiffuse(int index, const osg::Vec4& value)
        {
            // Deal with negative lights (negative diffuse) by passing a sign bit in the unused alpha component
            auto positiveColor = value;
            unsigned int signBit = 1;
            if (value[0] < 0)
            {
                positiveColor *= -1.0;
                signBit = ~0u;
            }
            unsigned int packedColor = asRGBA(positiveColor);
            std::memcpy(&(*mData)[getOffset(index, Diffuse)], &packedColor, sizeof(unsigned int));
            std::memcpy(&(*mData)[getOffset(index, DiffuseSign)], &signBit, sizeof(unsigned int));
        }

        void setAmbient(int index, const osg::Vec4& value)
        {
            unsigned int packed = asRGBA(value);
            std::memcpy(&(*mData)[getOffset(index, Ambient)], &packed, sizeof(unsigned int));
        }

        void setSpecular(int index, const osg::Vec4& value)
        {
            unsigned int packed = asRGBA(value);
            std::memcpy(&(*mData)[getOffset(index, Specular)], &packed, sizeof(unsigned int));
        }

        void setPosition(int index, const osg::Vec4& value)
        {
            std::memcpy(&(*mData)[getOffset(index, Position)], value.ptr(), sizeof(osg::Vec4f));
        }

        void setAttenuationRadius(int index, const osg::Vec4& value)
        {
            std::memcpy(&(*mData)[getOffset(index, AttenuationRadius)], value.ptr(), sizeof(osg::Vec4f));
        }

        auto& getData()
        {
            return mData;
        }

        void dirty()
        {
            mData->dirty();
        }

        static constexpr int queryBlockSize(int sz)
        {
            return 3 * osg::Vec4::num_components * sizeof(GLfloat) * sz;
        }

        void setCachedSunPos(const osg::Vec4& pos)
        {
            mCachedSunPos = pos;
        }

        void uploadCachedSunPos(const osg::Matrix& viewMat)
        {
            osg::Vec4 viewPos = mCachedSunPos * viewMat;
            std::memcpy(&(*mData)[getOffset(0, Position)], viewPos.ptr(), sizeof(osg::Vec4f));
        }

        unsigned int asRGBA(const osg::Vec4& value) const
        {
            return mEndian == osg::BigEndian ? value.asABGR() : value.asRGBA();
        }

        int getOffset(int index, LayoutOffset slot) const
        {
            return mOffsets.get(index, slot);
        }

        void configureLayout(int offsetColors, int offsetPosition, int offsetAttenuationRadius, int size, int stride)
        {
            const Offsets offsets(offsetColors, offsetPosition, offsetAttenuationRadius, stride);

            // Copy cloned data using current layout into current data using new layout.
            // This allows to preserve osg::FloatArray buffer object in mData.
            const auto data = mData->asVector();
            mData->resizeArray(static_cast<unsigned>(size));
            for (int i = 0; i < mCount; ++i)
            {
                std::memcpy(&(*mData)[offsets.get(i, Diffuse)], data.data() + getOffset(i, Diffuse), sizeof(osg::Vec4f));
                std::memcpy(&(*mData)[offsets.get(i, Position)], data.data() + getOffset(i, Position), sizeof(osg::Vec4f));
                std::memcpy(&(*mData)[offsets.get(i, AttenuationRadius)], data.data() + getOffset(i, AttenuationRadius), sizeof(osg::Vec4f));
            }
            mOffsets = offsets;
        }

    private:
        class Offsets
        {
            public:
                Offsets()
                    : mStride(12)
                {
                    mValues[Diffuse] = 0;
                    mValues[Ambient] = 1;
                    mValues[Specular] = 2;
                    mValues[DiffuseSign] = 3;
                    mValues[Position] = 4;
                    mValues[AttenuationRadius] = 8;
                }

                Offsets(int offsetColors, int offsetPosition, int offsetAttenuationRadius, int stride)
                    : mStride((offsetAttenuationRadius + sizeof(GLfloat) * osg::Vec4::num_components + stride) / 4)
                {
                    constexpr auto sizeofFloat = sizeof(GLfloat);
                    const auto diffuseOffset = offsetColors / sizeofFloat;

                    mValues[Diffuse] = diffuseOffset;
                    mValues[Ambient] = diffuseOffset + 1;
                    mValues[Specular] = diffuseOffset + 2;
                    mValues[DiffuseSign] = diffuseOffset + 3;
                    mValues[Position] = offsetPosition / sizeofFloat;
                    mValues[AttenuationRadius] = offsetAttenuationRadius / sizeofFloat;
                }

                int get(int index, LayoutOffset slot) const
                {
                    return mStride * index + mValues[slot];
                }

            private:
                int mStride;
                std::array<int, 6> mValues;
        };

        osg::ref_ptr<osg::FloatArray> mData;
        osg::Endian mEndian;
        int mCount;
        Offsets mOffsets;
        osg::Vec4 mCachedSunPos;
    };

    class LightStateCache
    {
    public:
        std::vector<osg::Light*> lastAppliedLight;
    };

    LightStateCache* getLightStateCache(size_t contextid, size_t size = 8)
    {
        static std::vector<LightStateCache> cacheVector;
        if (cacheVector.size() < contextid+1)
            cacheVector.resize(contextid+1);
        cacheVector[contextid].lastAppliedLight.resize(size);
        return &cacheVector[contextid];
    }

    void configureStateSetSunOverride(LightManager* lightManager, const osg::Light* light, osg::StateSet* stateset, int mode)
    {
        auto method = lightManager->getLightingMethod();
        switch (method)
        {
        case LightingMethod::FFP:
            {
                break;
            }
        case LightingMethod::PerObjectUniform:
            {
                osg::Matrixf lightMat;
                configurePosition(lightMat, light->getPosition());
                configureAmbient(lightMat, light->getAmbient());
                configureDiffuse(lightMat, light->getDiffuse());
                configureSpecular(lightMat, light->getSpecular());

                osg::ref_ptr<osg::Uniform> uni = new osg::Uniform(osg::Uniform::FLOAT_MAT4, "LightBuffer", lightManager->getMaxLights());
                uni->setElement(0, lightMat);
                stateset->addUniform(uni, mode);
                break;
            }
        case LightingMethod::SingleUBO:
            {
                osg::ref_ptr<LightBuffer> buffer = new LightBuffer(lightManager->getMaxLightsInScene());

                buffer->setDiffuse(0, light->getDiffuse());
                buffer->setAmbient(0, light->getAmbient());
                buffer->setSpecular(0, light->getSpecular());
                buffer->setPosition(0, light->getPosition());

                osg::ref_ptr<osg::UniformBufferObject> ubo = new osg::UniformBufferObject;
                buffer->getData()->setBufferObject(ubo);
#if OSG_VERSION_GREATER_OR_EQUAL(3,5,7)
                osg::ref_ptr<osg::UniformBufferBinding> ubb = new osg::UniformBufferBinding(static_cast<int>(Shader::UBOBinding::LightBuffer), buffer->getData(), 0, buffer->getData()->getTotalDataSize());
#else
                osg::ref_ptr<osg::UniformBufferBinding> ubb = new osg::UniformBufferBinding(static_cast<int>(Shader::UBOBinding::LightBuffer), ubo, 0, buffer->getData()->getTotalDataSize());
#endif
                stateset->setAttributeAndModes(ubb, mode);

                break;
            }
        }
    }

    class DisableLight : public osg::StateAttribute
    {
    public:
        DisableLight() : mIndex(0) {}
        DisableLight(int index) : mIndex(index) {}

        DisableLight(const DisableLight& copy,const osg::CopyOp& copyop=osg::CopyOp::SHALLOW_COPY)
            : osg::StateAttribute(copy,copyop), mIndex(copy.mIndex) {}

        osg::Object* cloneType() const override { return new DisableLight(mIndex); }
        osg::Object* clone(const osg::CopyOp& copyop) const override { return new DisableLight(*this,copyop); }
        bool isSameKindAs(const osg::Object* obj) const override { return dynamic_cast<const DisableLight *>(obj)!=nullptr; }
        const char* libraryName() const override { return "SceneUtil"; }
        const char* className() const override { return "DisableLight"; }
        Type getType() const override { return LIGHT; }

        unsigned int getMember() const override
        {
            return mIndex;
        }

        bool getModeUsage(ModeUsage & usage) const override
        {
            usage.usesMode(GL_LIGHT0 + mIndex);
            return true;
        }

        int compare(const StateAttribute &sa) const override
        {
            throw std::runtime_error("DisableLight::compare: unimplemented");
        }

        void apply(osg::State& state) const override
        {
            int lightNum = GL_LIGHT0 + mIndex;
            glLightfv(lightNum, GL_AMBIENT, mNullptr.ptr());
            glLightfv(lightNum, GL_DIFFUSE, mNullptr.ptr());
            glLightfv(lightNum, GL_SPECULAR, mNullptr.ptr());

            LightStateCache* cache = getLightStateCache(state.getContextID());
            cache->lastAppliedLight[mIndex] = nullptr;
        }

    private:
        size_t mIndex;
        osg::Vec4f mNullptr;
    };

    class FFPLightStateAttribute : public osg::StateAttribute
    {
    public:
        FFPLightStateAttribute() : mIndex(0) {}
        FFPLightStateAttribute(size_t index, const std::vector<osg::ref_ptr<osg::Light> >& lights) : mIndex(index), mLights(lights) {}

        FFPLightStateAttribute(const FFPLightStateAttribute& copy,const osg::CopyOp& copyop=osg::CopyOp::SHALLOW_COPY)
            : osg::StateAttribute(copy,copyop), mIndex(copy.mIndex), mLights(copy.mLights) {}

        unsigned int getMember() const override
        {
            return mIndex;
        }

        bool getModeUsage(ModeUsage & usage) const override
        {
            for (size_t i = 0; i < mLights.size(); ++i)
                usage.usesMode(GL_LIGHT0 + mIndex + i);
            return true;
        }

        int compare(const StateAttribute &sa) const override
        {
            throw std::runtime_error("FFPLightStateAttribute::compare: unimplemented");
        }

        META_StateAttribute(NifOsg, FFPLightStateAttribute, osg::StateAttribute::LIGHT)

        void apply(osg::State& state) const override
        {
            if (mLights.empty())
                return;
            osg::Matrix modelViewMatrix = state.getModelViewMatrix();

            state.applyModelViewMatrix(state.getInitialViewMatrix());

            LightStateCache* cache = getLightStateCache(state.getContextID());

            for (size_t i = 0; i < mLights.size(); ++i)
            {
                osg::Light* current = cache->lastAppliedLight[i+mIndex];
                if (current != mLights[i].get())
                {
                    applyLight((GLenum)((int)GL_LIGHT0 + i + mIndex), mLights[i].get());
                    cache->lastAppliedLight[i+mIndex] = mLights[i].get();
                }
            }

            state.applyModelViewMatrix(modelViewMatrix);
        }

        void applyLight(GLenum lightNum, const osg::Light* light) const
        {
            glLightfv(lightNum, GL_AMBIENT, light->getAmbient().ptr());
            glLightfv(lightNum, GL_DIFFUSE, light->getDiffuse().ptr());
            glLightfv(lightNum, GL_SPECULAR, light->getSpecular().ptr());
            glLightfv(lightNum, GL_POSITION, light->getPosition().ptr());
            // TODO: enable this once spot lights are supported
            // need to transform SPOT_DIRECTION by the world matrix?
            //glLightfv(lightNum, GL_SPOT_DIRECTION, light->getDirection().ptr());
            //glLightf(lightNum, GL_SPOT_EXPONENT, light->getSpotExponent());
            //glLightf(lightNum, GL_SPOT_CUTOFF, light->getSpotCutoff());
            glLightf(lightNum, GL_CONSTANT_ATTENUATION, light->getConstantAttenuation());
            glLightf(lightNum, GL_LINEAR_ATTENUATION, light->getLinearAttenuation());
            glLightf(lightNum, GL_QUADRATIC_ATTENUATION, light->getQuadraticAttenuation());
        }

    private:
        size_t mIndex;
        std::vector<osg::ref_ptr<osg::Light>> mLights;
    };

    LightManager* findLightManager(const osg::NodePath& path)
    {
        for (size_t i = 0; i < path.size(); ++i)
        {
            if (LightManager* lightManager = dynamic_cast<LightManager*>(path[i]))
                return lightManager;
        }
        return nullptr;
    }

    class LightStateAttributePerObjectUniform : public osg::StateAttribute
    {
    public:
        LightStateAttributePerObjectUniform() : mLightManager(nullptr) {}
        LightStateAttributePerObjectUniform(const std::vector<osg::ref_ptr<osg::Light>>& lights, LightManager* lightManager) :  mLights(lights), mLightManager(lightManager) {}

        LightStateAttributePerObjectUniform(const LightStateAttributePerObjectUniform& copy,const osg::CopyOp& copyop=osg::CopyOp::SHALLOW_COPY)
            : osg::StateAttribute(copy,copyop), mLights(copy.mLights), mLightManager(copy.mLightManager) {}

        int compare(const StateAttribute &sa) const override
        {
            throw std::runtime_error("LightStateAttributePerObjectUniform::compare: unimplemented");
        }

        META_StateAttribute(NifOsg, LightStateAttributePerObjectUniform, osg::StateAttribute::LIGHT)

        void resize(int numLights)
        {
            mLights.resize(std::min(static_cast<size_t>(numLights), mLights.size()));
        }

        void apply(osg::State &state) const override
        {
            osg::StateSet* stateSet = mLightManager->getStateSet();
            if (!stateSet)
                return;

            auto* lightUniform = stateSet->getUniform("LightBuffer");
            for (size_t i = 0; i < mLights.size(); ++i)
            {
                auto light = mLights[i];
                osg::Matrixf lightMat;

                configurePosition(lightMat, light->getPosition() * state.getInitialViewMatrix());
                configureAmbient(lightMat, light->getAmbient());
                configureDiffuse(lightMat, light->getDiffuse());
                configureAttenuation(lightMat, light->getConstantAttenuation(), light->getLinearAttenuation(), light->getQuadraticAttenuation(), getLightRadius(light));

                lightUniform->setElement(i+1, lightMat);
            }

            auto sun = mLightManager->getSunlightBuffer(state.getFrameStamp()->getFrameNumber());
            configurePosition(sun, osg::Vec4(sun(0,0), sun(0,1), sun(0,2), 0.0) * state.getInitialViewMatrix());
            lightUniform->setElement(0, sun);

            lightUniform->dirty();
        }

    private:
        std::vector<osg::ref_ptr<osg::Light>> mLights;
        LightManager* mLightManager;
    };

    struct StateSetGenerator
    {
        LightManager* mLightManager;

        virtual ~StateSetGenerator() {}

        virtual osg::ref_ptr<osg::StateSet> generate(const LightManager::LightList& lightList, size_t frameNum) = 0;

        virtual void update(osg::StateSet* stateset, const LightManager::LightList& lightList, size_t frameNum) {}
    };

    struct StateSetGeneratorFFP : StateSetGenerator
    {
        osg::ref_ptr<osg::StateSet> generate(const LightManager::LightList& lightList, size_t frameNum) override
        {
            osg::ref_ptr<osg::StateSet> stateset = new osg::StateSet;

            std::vector<osg::ref_ptr<osg::Light>> lights;
            lights.reserve(lightList.size());
            for (size_t i = 0; i < lightList.size(); ++i)
                lights.emplace_back(lightList[i]->mLightSource->getLight(frameNum));

            // the first light state attribute handles the actual state setting for all lights
            // it's best to batch these up so that we don't need to touch the modelView matrix more than necessary
            // don't use setAttributeAndModes, that does not support light indices!
            stateset->setAttribute(new FFPLightStateAttribute(mLightManager->getStartLight(), std::move(lights)), osg::StateAttribute::ON);

            for (size_t i = 0; i < lightList.size(); ++i)
                stateset->setMode(GL_LIGHT0 + mLightManager->getStartLight() + i, osg::StateAttribute::ON);

            // need to push some dummy attributes to ensure proper state tracking
            // lights need to reset to their default when the StateSet is popped
            for (size_t i = 1; i < lightList.size(); ++i)
                stateset->setAttribute(mLightManager->getDummies()[i + mLightManager->getStartLight()].get(), osg::StateAttribute::ON);

            return stateset;
        }
    };

    struct StateSetGeneratorSingleUBO : StateSetGenerator
    {
        osg::ref_ptr<osg::StateSet> generate(const LightManager::LightList& lightList, size_t frameNum) override
        {
            osg::ref_ptr<osg::StateSet> stateset = new osg::StateSet;

            osg::ref_ptr<osg::IntArray> indices = new osg::IntArray(mLightManager->getMaxLights());
            osg::ref_ptr<osg::Uniform> indicesUni = new osg::Uniform(osg::Uniform::Type::INT, "PointLightIndex", indices->size());
            int pointCount = 0;

            for (size_t i = 0; i < lightList.size(); ++i)
            {
                int bufIndex = mLightManager->getLightIndexMap(frameNum)[lightList[i]->mLightSource->getId()];
                indices->at(pointCount++) = bufIndex;
            }
            indicesUni->setArray(indices);
            stateset->addUniform(indicesUni);
            stateset->addUniform(new osg::Uniform("PointLightCount", pointCount));

            return stateset;
        }

        // Cached statesets must be revalidated in case the light indices change. There is no actual link between
        // a light's ID and the buffer index it will eventually be assigned (or reassigned) to.
        void update(osg::StateSet* stateset, const LightManager::LightList& lightList, size_t frameNum) override
        {
            int newCount = 0;
            int oldCount;

            auto uOldArray = stateset->getUniform("PointLightIndex");
            auto uOldCount = stateset->getUniform("PointLightCount");

            uOldCount->get(oldCount);

            // max lights count can change during runtime
            oldCount = std::min(mLightManager->getMaxLights(), oldCount);

            auto& lightData = mLightManager->getLightIndexMap(frameNum);

            for (int i = 0; i < oldCount; ++i)
            {
                auto* lightSource = lightList[i]->mLightSource;
                auto it = lightData.find(lightSource->getId());
                if (it != lightData.end())
                    uOldArray->setElement(newCount++, it->second);
            }

            uOldArray->dirty();
            uOldCount->set(newCount);
        }
    };

    struct StateSetGeneratorPerObjectUniform : StateSetGenerator
    {
        osg::ref_ptr<osg::StateSet> generate(const LightManager::LightList& lightList, size_t frameNum) override
        {
            osg::ref_ptr<osg::StateSet> stateset = new osg::StateSet;

            std::vector<osg::ref_ptr<osg::Light>> lights(lightList.size());

            for (size_t i = 0; i < lightList.size(); ++i)
            {
                auto* light = lightList[i]->mLightSource->getLight(frameNum);
                lights[i] = light;
                setLightRadius(light, lightList[i]->mLightSource->getRadius());
            }

            stateset->setAttributeAndModes(new LightStateAttributePerObjectUniform(std::move(lights), mLightManager), osg::StateAttribute::ON);

            stateset->addUniform(new osg::Uniform("PointLightCount", static_cast<int>(lightList.size() + 1)));

            return stateset;
        }
    };

    // Set on a LightSource. Adds the light source to its light manager for the current frame.
    // This allows us to keep track of the current lights in the scene graph without tying creation & destruction to the manager.
    class CollectLightCallback : public osg::NodeCallback
    {
    public:
        CollectLightCallback()
            : mLightManager(nullptr) { }

        CollectLightCallback(const CollectLightCallback& copy, const osg::CopyOp& copyop)
            : osg::NodeCallback(copy, copyop)
            , mLightManager(nullptr) { }

        META_Object(SceneUtil, SceneUtil::CollectLightCallback)

        void operator()(osg::Node* node, osg::NodeVisitor* nv) override
        {
            if (!mLightManager)
            {
                mLightManager = findLightManager(nv->getNodePath());

                if (!mLightManager)
                    throw std::runtime_error("can't find parent LightManager");
            }

            mLightManager->addLight(static_cast<LightSource*>(node), osg::computeLocalToWorld(nv->getNodePath()), nv->getTraversalNumber());

            traverse(node, nv);
        }

    private:
        LightManager* mLightManager;
    };

    // Set on a LightManager. Clears the data from the previous frame.
    class LightManagerUpdateCallback : public osg::NodeCallback
    {
    public:
        LightManagerUpdateCallback()
            { }

        LightManagerUpdateCallback(const LightManagerUpdateCallback& copy, const osg::CopyOp& copyop)
            : osg::NodeCallback(copy, copyop)
            { }

        META_Object(SceneUtil, LightManagerUpdateCallback)

        void operator()(osg::Node* node, osg::NodeVisitor* nv) override
        {
            LightManager* lightManager = static_cast<LightManager*>(node);
            lightManager->update(nv->getTraversalNumber());

            traverse(node, nv);
        }
    };

    class LightManagerCullCallback : public osg::NodeCallback
    {
    public:
        LightManagerCullCallback(LightManager* lightManager) : mLightManager(lightManager), mLastFrameNumber(0) {}

        void operator()(osg::Node* node, osg::NodeVisitor* nv) override
        {
            osgUtil::CullVisitor* cv = static_cast<osgUtil::CullVisitor*>(nv);

            if (mLastFrameNumber != cv->getTraversalNumber())
            {
                mLastFrameNumber = cv->getTraversalNumber();

                if (mLightManager->getLightingMethod() == LightingMethod::SingleUBO)
                {
                    auto stateset = mLightManager->getStateSet();
                    auto bo = mLightManager->getLightBuffer(mLastFrameNumber);

#if OSG_VERSION_GREATER_OR_EQUAL(3,5,7)
                    osg::ref_ptr<osg::UniformBufferBinding> ubb = new osg::UniformBufferBinding(static_cast<int>(Shader::UBOBinding::LightBuffer), bo->getData(), 0, bo->getData()->getTotalDataSize());
#else
                    osg::ref_ptr<osg::UniformBufferBinding> ubb = new osg::UniformBufferBinding(static_cast<int>(Shader::UBOBinding::LightBuffer), bo->getData()->getBufferObject(), 0, bo->getData()->getTotalDataSize());
#endif
                    stateset->setAttributeAndModes(ubb, osg::StateAttribute::ON);
                }

                auto sun = mLightManager->getSunlight();

                if (sun)
                {
                    // we must defer uploading the transformation to view-space position to deal with different cameras (e.g. reflection RTT).
                    if (mLightManager->getLightingMethod() == LightingMethod::PerObjectUniform)
                    {
                        osg::Matrixf lightMat;
                        configurePosition(lightMat, sun->getPosition());
                        configureAmbient(lightMat, sun->getAmbient());
                        configureDiffuse(lightMat, sun->getDiffuse());
                        configureSpecular(lightMat, sun->getSpecular());
                        mLightManager->setSunlightBuffer(lightMat, mLastFrameNumber);
                    }
                    else
                    {
                        auto buf = mLightManager->getLightBuffer(mLastFrameNumber);

                        buf->setCachedSunPos(sun->getPosition());
                        buf->setAmbient(0, sun->getAmbient());
                        buf->setDiffuse(0, sun->getDiffuse());
                        buf->setSpecular(0, sun->getSpecular());
                    }
                }
            }

            traverse(node, nv);
        }

    private:
        LightManager* mLightManager;
        size_t mLastFrameNumber;
    };

    class LightManagerStateAttribute : public osg::StateAttribute
    {
    public:
        LightManagerStateAttribute()
            : mLightManager(nullptr)
            , mInitLayout(false)
        {
        }

        LightManagerStateAttribute(LightManager* lightManager)
        : mLightManager(lightManager)
        , mDummyProgram(new osg::Program)
        , mInitLayout(false)
        {
            static const std::string dummyVertSource = generateDummyShader(mLightManager->getMaxLightsInScene());

            // Needed to query the layout of the buffer object. The layout specifier needed to use the std140 layout is not reliably
            // available, regardless of extensions, until GLSL 140.
            mDummyProgram->addShader(new osg::Shader(osg::Shader::VERTEX, dummyVertSource));
            mDummyProgram->addBindUniformBlock("LightBufferBinding", static_cast<int>(Shader::UBOBinding::LightBuffer));
        }

        LightManagerStateAttribute(const LightManagerStateAttribute& copy, const osg::CopyOp& copyop=osg::CopyOp::SHALLOW_COPY)
            : osg::StateAttribute(copy,copyop), mLightManager(copy.mLightManager), mInitLayout(copy.mInitLayout) {}

        int compare(const StateAttribute &sa) const override
        {
            throw std::runtime_error("LightManagerStateAttribute::compare: unimplemented");
        }

        META_StateAttribute(NifOsg, LightManagerStateAttribute, osg::StateAttribute::LIGHT)

        void initSharedLayout(osg::GLExtensions* ext, int handle) const
        {
            constexpr std::array<unsigned int, 1> index = { static_cast<unsigned int>(Shader::UBOBinding::LightBuffer) };
            int totalBlockSize = -1;
            int stride = -1;

            ext->glGetActiveUniformBlockiv(handle, 0, GL_UNIFORM_BLOCK_DATA_SIZE, &totalBlockSize);
            ext->glGetActiveUniformsiv(handle, index.size(), index.data(), GL_UNIFORM_ARRAY_STRIDE, &stride);

            std::array<const char*, 3> names = {
                "LightBuffer[0].packedColors",
                "LightBuffer[0].position",
                "LightBuffer[0].attenuation",
            };
            std::vector<unsigned int> indices(names.size());
            std::vector<int> offsets(names.size());

            ext->glGetUniformIndices(handle, names.size(), names.data(), indices.data());
            ext->glGetActiveUniformsiv(handle, indices.size(), indices.data(), GL_UNIFORM_OFFSET, offsets.data());

            for (int i = 0; i < 2; ++i)
                mLightManager->getLightBuffer(i)->configureLayout(offsets[0], offsets[1], offsets[2], totalBlockSize, stride);
        }

        void apply(osg::State& state) const override
        {
            if (!mInitLayout)
            {
                mDummyProgram->apply(state);
                auto handle = mDummyProgram->getPCP(state)->getHandle();
                auto* ext = state.get<osg::GLExtensions>();

                int activeUniformBlocks = 0;
                ext->glGetProgramiv(handle, GL_ACTIVE_UNIFORM_BLOCKS, &activeUniformBlocks);

                // wait until the UBO binding is created
                if (activeUniformBlocks > 0)
                {
                    initSharedLayout(ext, handle);
                    mInitLayout = true;
                }
            }
            mLightManager->getLightBuffer(state.getFrameStamp()->getFrameNumber())->uploadCachedSunPos(state.getInitialViewMatrix());
            mLightManager->getLightBuffer(state.getFrameStamp()->getFrameNumber())->dirty();
        }

    private:

        std::string generateDummyShader(int maxLightsInScene)
        {
            const std::string define = "@maxLightsInScene";

            std::string shader = R"GLSL(
                #version 120
                #extension GL_ARB_uniform_buffer_object : require
                struct LightData {
                   ivec4 packedColors;
                   vec4 position;
                   vec4 attenuation;
                };
                uniform LightBufferBinding {
                   LightData LightBuffer[@maxLightsInScene];
                };
                void main()
                {
                    gl_Position = vec4(0.0);
                }
            )GLSL";

            shader.replace(shader.find(define), define.length(), std::to_string(maxLightsInScene));
            return shader;
        }

        LightManager* mLightManager;
        osg::ref_ptr<osg::Program> mDummyProgram;
        mutable bool mInitLayout;
    };

    const std::unordered_map<std::string, LightingMethod> LightManager::mLightingMethodSettingMap = {
         {"legacy", LightingMethod::FFP}
        ,{"shaders compatibility", LightingMethod::PerObjectUniform}
        ,{"shaders", LightingMethod::SingleUBO}
    };

    LightingMethod LightManager::getLightingMethodFromString(const std::string& value)
    {
        auto it = LightManager::mLightingMethodSettingMap.find(value);
        if (it != LightManager::mLightingMethodSettingMap.end())
            return it->second;

        constexpr const char* fallback = "shaders compatibility";
        Log(Debug::Warning) << "Unknown lighting method '" << value << "', returning fallback '" << fallback << "'";
        return LightingMethod::PerObjectUniform;
    }

    std::string LightManager::getLightingMethodString(LightingMethod method)
    {
        for (const auto& p : LightManager::mLightingMethodSettingMap)
            if (p.second == method)
                return p.first;
        return "";
    }

    LightManager::~LightManager()
    {
        getOrCreateStateSet()->removeAttribute(osg::StateAttribute::LIGHT);
    }

    LightManager::LightManager(bool ffp)
        : mStartLight(0)
        , mLightingMask(~0u)
        , mSun(nullptr)
        , mPointLightRadiusMultiplier(1.f)
        , mPointLightFadeEnd(0.f)
        , mPointLightFadeStart(0.f)
    {
        osg::GLExtensions* exts = osg::GLExtensions::Get(0, false);
        bool supportsUBO = exts && exts->isUniformBufferObjectSupported;
        bool supportsGPU4 = exts && exts->isGpuShader4Supported;

        mSupported[static_cast<int>(LightingMethod::FFP)] = true;
        mSupported[static_cast<int>(LightingMethod::PerObjectUniform)] = true;
        mSupported[static_cast<int>(LightingMethod::SingleUBO)] = supportsUBO && supportsGPU4;

        setUpdateCallback(new LightManagerUpdateCallback);

        if (ffp)
        {
            initFFP(mFFPMaxLights);
            return;
        }

        std::string lightingMethodString = Settings::Manager::getString("lighting method", "Shaders");
        auto lightingMethod = LightManager::getLightingMethodFromString(lightingMethodString);

        static bool hasLoggedWarnings = false;

        if (lightingMethod == LightingMethod::SingleUBO && !hasLoggedWarnings)
        {
            if (!supportsUBO)
                Log(Debug::Warning) << "GL_ARB_uniform_buffer_object not supported: switching to shader compatibility lighting mode";
            if (!supportsGPU4)
                Log(Debug::Warning) << "GL_EXT_gpu_shader4 not supported: switching to shader compatibility lighting mode";
            hasLoggedWarnings = true;
        }

        int targetLights = std::clamp(Settings::Manager::getInt("max lights", "Shaders"), mMaxLightsLowerLimit, mMaxLightsUpperLimit);

        if (!supportsUBO || !supportsGPU4 || lightingMethod == LightingMethod::PerObjectUniform)
            initPerObjectUniform(targetLights);
        else
            initSingleUBO(targetLights);

        updateSettings();

        getOrCreateStateSet()->addUniform(new osg::Uniform("PointLightCount", 0));

        addCullCallback(new LightManagerCullCallback(this));
    }

    LightManager::LightManager(const LightManager &copy, const osg::CopyOp &copyop)
        : osg::Group(copy, copyop)
        , mStartLight(copy.mStartLight)
        , mLightingMask(copy.mLightingMask)
        , mSun(copy.mSun)
        , mLightingMethod(copy.mLightingMethod)
        , mPointLightRadiusMultiplier(copy.mPointLightRadiusMultiplier)
        , mPointLightFadeEnd(copy.mPointLightFadeEnd)
        , mPointLightFadeStart(copy.mPointLightFadeStart)
        , mMaxLights(copy.mMaxLights)
    {
    }

    LightingMethod LightManager::getLightingMethod() const
    {
        return mLightingMethod;
    }

    bool LightManager::usingFFP() const
    {
        return mLightingMethod == LightingMethod::FFP;
    }

    int LightManager::getMaxLights() const
    {
        return mMaxLights;
    }

    void LightManager::setMaxLights(int value)
    {
        mMaxLights = value;
    }

    int LightManager::getMaxLightsInScene() const
    {
        static constexpr int max = 16384 / LightBuffer::queryBlockSize(1);
        return max;
    }

    Shader::ShaderManager::DefineMap LightManager::getLightDefines() const
    {
        Shader::ShaderManager::DefineMap defines;

        defines["maxLights"] = std::to_string(getMaxLights());
        defines["maxLightsInScene"] = std::to_string(getMaxLightsInScene());
        defines["lightingMethodFFP"] = getLightingMethod() == LightingMethod::FFP ? "1" : "0";
        defines["lightingMethodPerObjectUniform"] = getLightingMethod() == LightingMethod::PerObjectUniform ? "1" : "0";
        defines["lightingMethodUBO"] = getLightingMethod() == LightingMethod::SingleUBO ? "1" : "0";
        defines["useUBO"] = std::to_string(getLightingMethod() == LightingMethod::SingleUBO);
        // exposes bitwise operators
        defines["useGPUShader4"] = std::to_string(getLightingMethod() == LightingMethod::SingleUBO);
        defines["getLight"] = getLightingMethod() == LightingMethod::FFP ? "gl_LightSource" : "LightBuffer";
        defines["startLight"] =  getLightingMethod() == LightingMethod::SingleUBO ? "0" : "1";
        defines["endLight"] = getLightingMethod() == LightingMethod::FFP ? defines["maxLights"] : "PointLightCount";

        return defines;
    }

    void LightManager::processChangedSettings(const Settings::CategorySettingVector& changed)
    {
        updateSettings();
    }

    void LightManager::updateMaxLights()
    {
        if (usingFFP())
            return;

        int targetLights = std::clamp(Settings::Manager::getInt("max lights", "Shaders"), mMaxLightsLowerLimit, mMaxLightsUpperLimit);
        setMaxLights(targetLights);

        if (getLightingMethod() == LightingMethod::PerObjectUniform)
        {
            auto* prevUniform = getStateSet()->getUniform("LightBuffer");
            osg::ref_ptr<osg::Uniform> newUniform = new osg::Uniform(osg::Uniform::FLOAT_MAT4, "LightBuffer", getMaxLights());

            for (int i = 0; i < getMaxLights(); ++i)
            {
                osg::Matrixf prevLightData;
                prevUniform->getElement(i, prevLightData);
                newUniform->setElement(i, prevLightData);
            }

            getStateSet()->removeUniform(prevUniform);
            getStateSet()->addUniform(newUniform);

            for (int i = 0; i < 2; ++i)
            {
                for (auto& pair : mStateSetCache[i])
                    static_cast<LightStateAttributePerObjectUniform*>(pair.second->getAttribute(osg::StateAttribute::LIGHT))->resize(getMaxLights());
                mStateSetCache[i].clear();
            }
        }
        else
        {
            for (int i = 0; i < 2; ++i)
            {
                for (auto& pair : mStateSetCache[i])
                {
                    auto& stateset = pair.second;
                    osg::Uniform* uOldArray = stateset->getUniform("PointLightIndex");
                    osg::Uniform* uOldCount = stateset->getUniform("PointLightCount");

                    int prevCount;
                    uOldCount->get(prevCount);
                    int newCount = std::min(getMaxLights(), prevCount);
                    uOldCount->set(newCount);

                    osg::ref_ptr<osg::IntArray> newArray = uOldArray->getIntArray();
                    newArray->resize(newCount);

                    stateset->removeUniform(uOldArray);
                    stateset->addUniform(new osg::Uniform("PointLightIndex", newArray));
                }
                mStateSetCache[i].clear();
            }
        }
    }

    void LightManager::updateSettings()
    {
        if (getLightingMethod() == LightingMethod::FFP)
            return;

        mPointLightRadiusMultiplier = std::clamp(Settings::Manager::getFloat("light bounds multiplier", "Shaders"), 0.f, 5.f);

        mPointLightFadeEnd = std::max(0.f, Settings::Manager::getFloat("maximum light distance", "Shaders"));
        if (mPointLightFadeEnd > 0)
        {
            mPointLightFadeStart = std::clamp(Settings::Manager::getFloat("light fade start", "Shaders"), 0.f, 1.f);
            mPointLightFadeStart = mPointLightFadeEnd * mPointLightFadeStart;
        }
    }

    void LightManager::initFFP(int targetLights)
    {
        setLightingMethod(LightingMethod::FFP);
        setMaxLights(targetLights);

        for (int i = 0; i < getMaxLights(); ++i)
            mDummies.push_back(new FFPLightStateAttribute(i, std::vector<osg::ref_ptr<osg::Light>>()));
    }

    void LightManager::initPerObjectUniform(int targetLights)
    {
        auto* stateset = getOrCreateStateSet();

        setLightingMethod(LightingMethod::PerObjectUniform);
        setMaxLights(targetLights);

        // ensures sunlight element in our uniform array is updated when there are no point lights in scene
        stateset->setAttributeAndModes(new LightStateAttributePerObjectUniform({}, this), osg::StateAttribute::ON);
        stateset->addUniform(new osg::Uniform(osg::Uniform::FLOAT_MAT4, "LightBuffer", getMaxLights()));
    }

    void LightManager::initSingleUBO(int targetLights)
    {
        setLightingMethod(LightingMethod::SingleUBO);
        setMaxLights(targetLights);

        for (int i = 0; i < 2; ++i)
        {
            mLightBuffers[i] = new LightBuffer(getMaxLightsInScene());

            osg::ref_ptr<osg::UniformBufferObject> ubo = new osg::UniformBufferObject;
            ubo->setUsage(GL_STREAM_DRAW);

            mLightBuffers[i]->getData()->setBufferObject(ubo);
        }

        getOrCreateStateSet()->setAttribute(new LightManagerStateAttribute(this), osg::StateAttribute::ON);
    }

    void LightManager::setLightingMethod(LightingMethod method)
    {
        mLightingMethod = method;
        switch (method)
        {
        case LightingMethod::FFP:
            mStateSetGenerator = std::make_unique<StateSetGeneratorFFP>();
            break;
        case LightingMethod::SingleUBO:
            mStateSetGenerator = std::make_unique<StateSetGeneratorSingleUBO>();
            break;
        case LightingMethod::PerObjectUniform:
            mStateSetGenerator = std::make_unique<StateSetGeneratorPerObjectUniform>();
            break;
        }
        mStateSetGenerator->mLightManager = this;
    }

    void LightManager::setLightingMask(size_t mask)
    {
        mLightingMask = mask;
    }

    size_t LightManager::getLightingMask() const
    {
        return mLightingMask;
    }

    void LightManager::setStartLight(int start)
    {
        mStartLight = start;

        if (!usingFFP()) return;

        // Set default light state to zero
        // This is necessary because shaders don't respect glDisable(GL_LIGHTX) so in addition to disabling
        // we'll have to set a light state that has no visible effect
        for (int i = start; i < getMaxLights(); ++i)
        {
            osg::ref_ptr<DisableLight> defaultLight (new DisableLight(i));
            getOrCreateStateSet()->setAttributeAndModes(defaultLight, osg::StateAttribute::OFF);
        }
    }

    int LightManager::getStartLight() const
    {
        return mStartLight;
    }

    void LightManager::update(size_t frameNum)
    {
        getLightIndexMap(frameNum).clear();
        mLights.clear();
        mLightsInViewSpace.clear();

        // Do an occasional cleanup for orphaned lights.
        for (int i = 0; i < 2; ++i)
        {
            if (mStateSetCache[i].size() > 5000)
                mStateSetCache[i].clear();
        }
    }

    void LightManager::addLight(LightSource* lightSource, const osg::Matrixf& worldMat, size_t frameNum)
    {
        LightSourceTransform l;
        l.mLightSource = lightSource;
        l.mWorldMatrix = worldMat;
        osg::Vec3f pos = osg::Vec3f(worldMat.getTrans().x(), worldMat.getTrans().y(), worldMat.getTrans().z());
        lightSource->getLight(frameNum)->setPosition(osg::Vec4f(pos, 1.f));

        mLights.push_back(l);
    }

    void LightManager::setSunlight(osg::ref_ptr<osg::Light> sun)
    {
        if (usingFFP()) return;

        mSun = sun;
    }

    osg::ref_ptr<osg::Light> LightManager::getSunlight()
    {
        return mSun;
    }

    osg::ref_ptr<osg::StateSet> LightManager::getLightListStateSet(const LightList& lightList, size_t frameNum, const osg::RefMatrix* viewMatrix)
    {
        // possible optimization: return a StateSet containing all requested lights plus some extra lights (if a suitable one exists)
        size_t hash = 0;
        for (size_t i = 0; i < lightList.size(); ++i)
        {
            auto id = lightList[i]->mLightSource->getId();
            Misc::hashCombine(hash, id);

            if (getLightingMethod() != LightingMethod::SingleUBO)
                continue;

            if (getLightIndexMap(frameNum).find(id) != getLightIndexMap(frameNum).end())
                continue;

            int index = getLightIndexMap(frameNum).size() + 1;
            updateGPUPointLight(index, lightList[i]->mLightSource, frameNum, viewMatrix);
            getLightIndexMap(frameNum).emplace(lightList[i]->mLightSource->getId(), index);
        }

        auto& stateSetCache = mStateSetCache[frameNum%2];

        auto found = stateSetCache.find(hash);
        if (found != stateSetCache.end())
        {
            mStateSetGenerator->update(found->second, lightList, frameNum);
            return found->second;
        }

        auto stateset = mStateSetGenerator->generate(lightList, frameNum);
        stateSetCache.emplace(hash, stateset);
        return stateset;
    }

    const std::vector<LightManager::LightSourceViewBound>& LightManager::getLightsInViewSpace(osg::Camera *camera, const osg::RefMatrix* viewMatrix, size_t frameNum)
    {
        osg::observer_ptr<osg::Camera> camPtr (camera);
        auto it = mLightsInViewSpace.find(camPtr);

        if (it == mLightsInViewSpace.end())
        {
            it = mLightsInViewSpace.insert(std::make_pair(camPtr, LightSourceViewBoundCollection())).first;

            bool isReflection = isReflectionCamera(camera);
        
            for (const auto& transform : mLights)
            {
                osg::Matrixf worldViewMat = transform.mWorldMatrix * (*viewMatrix);

                float radius = transform.mLightSource->getRadius();

                osg::BoundingSphere viewBound = osg::BoundingSphere(osg::Vec3f(0,0,0), radius * mPointLightRadiusMultiplier);
                transformBoundingSphere(worldViewMat, viewBound);

                if (!isReflection && mPointLightFadeEnd != 0.f)
                {
                    const float fadeDelta = mPointLightFadeEnd - mPointLightFadeStart;
                    float fade = 1 - std::clamp((viewBound.center().length() - mPointLightFadeStart) / fadeDelta, 0.f, 1.f);
                    if (fade == 0.f)
                        continue;

                    auto* light = transform.mLightSource->getLight(frameNum);
                    light->setDiffuse(light->getDiffuse() * fade);
                }

                LightSourceViewBound l;
                l.mLightSource = transform.mLightSource;
                l.mViewBound = viewBound;
                it->second.push_back(l);
            }
        }

        if (getLightingMethod() == LightingMethod::SingleUBO)
        {
            if (it->second.size() > static_cast<size_t>(getMaxLightsInScene() - 1))
            {
                auto sorter = [] (const LightSourceViewBound& left, const LightSourceViewBound& right) {
                    return left.mViewBound.center().length2() - left.mViewBound.radius2() < right.mViewBound.center().length2() - right.mViewBound.radius2();
                };
                std::sort(it->second.begin() + 1, it->second.end(), sorter);
                it->second.erase((it->second.begin() + 1) + (getMaxLightsInScene() - 2), it->second.end());
            }
        }

        return it->second;
    }

    void LightManager::updateGPUPointLight(int index, LightSource* lightSource, size_t frameNum,const osg::RefMatrix* viewMatrix)
    {
        auto* light = lightSource->getLight(frameNum);
        auto& buf = getLightBuffer(frameNum);
        buf->setDiffuse(index, light->getDiffuse());
        buf->setAmbient(index, light->getAmbient());
        buf->setAttenuationRadius(index, osg::Vec4(light->getConstantAttenuation(), light->getLinearAttenuation(), light->getQuadraticAttenuation(), lightSource->getRadius()));
        buf->setPosition(index, light->getPosition() * (*viewMatrix));
    }

    LightSource::LightSource()
        : mRadius(0.f)
        , mActorFade(1.f)
    {
        setUpdateCallback(new CollectLightCallback);
        mId = sLightId++;
    }

    LightSource::LightSource(const LightSource &copy, const osg::CopyOp &copyop)
        : osg::Node(copy, copyop)
        , mRadius(copy.mRadius)
        , mActorFade(copy.mActorFade)
    {
        mId = sLightId++;

        for (int i = 0; i < 2; ++i)
            mLight[i] = new osg::Light(*copy.mLight[i].get(), copyop);
    }

    void LightListCallback::operator()(osg::Node *node, osg::NodeVisitor *nv)
    {
        osgUtil::CullVisitor* cv = static_cast<osgUtil::CullVisitor*>(nv);

        bool pushedState = pushLightState(node, cv);
        traverse(node, nv);
        if (pushedState)
            cv->popStateSet();
    }

    bool LightListCallback::pushLightState(osg::Node *node, osgUtil::CullVisitor *cv)
    {
        if (!mLightManager)
        {
            mLightManager = findLightManager(cv->getNodePath());
            if (!mLightManager)
                return false;
        }

        if (!(cv->getTraversalMask() & mLightManager->getLightingMask()))
            return false;

        // Possible optimizations:
        // - cull list of lights by the camera frustum
        // - organize lights in a quad tree


        // update light list if necessary
        // makes sure we don't update it more than once per frame when rendering with multiple cameras
        if (mLastFrameNumber != cv->getTraversalNumber())
        {
            mLastFrameNumber = cv->getTraversalNumber();

            // Don't use Camera::getViewMatrix, that one might be relative to another camera!
            const osg::RefMatrix* viewMatrix = cv->getCurrentRenderStage()->getInitialViewMatrix();
            const std::vector<LightManager::LightSourceViewBound>& lights = mLightManager->getLightsInViewSpace(cv->getCurrentCamera(), viewMatrix, mLastFrameNumber);

            // get the node bounds in view space
            // NB do not node->getBound() * modelView, that would apply the node's transformation twice
            osg::BoundingSphere nodeBound;
            osg::Transform* transform = node->asTransform();
            if (transform)
            {
                for (size_t i = 0; i < transform->getNumChildren(); ++i)
                    nodeBound.expandBy(transform->getChild(i)->getBound());
            }
            else
                nodeBound = node->getBound();
            osg::Matrixf mat = *cv->getModelViewMatrix();
            transformBoundingSphere(mat, nodeBound);

            mLightList.clear();
            for (size_t i = 0; i < lights.size(); ++i)
            {
                const LightManager::LightSourceViewBound& l = lights[i];

                if (mIgnoredLightSources.count(l.mLightSource))
                    continue;

                if (l.mViewBound.intersects(nodeBound))
                    mLightList.push_back(&l);
            }
        }
        if (!mLightList.empty())
        {
            size_t maxLights = mLightManager->getMaxLights() - mLightManager->getStartLight();

            osg::StateSet* stateset = nullptr;

            if (mLightList.size() > maxLights)
            {
                // remove lights culled by this camera
                LightManager::LightList lightList = mLightList;
                for (auto it = lightList.begin(); it != lightList.end() && lightList.size() > maxLights;)
                {
                    osg::CullStack::CullingStack& stack = cv->getModelViewCullingStack();

                    osg::BoundingSphere bs = (*it)->mViewBound;
                    bs._radius = bs._radius * 2.0;
                    osg::CullingSet& cullingSet = stack.front();
                    if (cullingSet.isCulled(bs))
                    {
                        it = lightList.erase(it);
                        continue;
                    }
                    else
                        ++it;
                }

                if (lightList.size() > maxLights)
                {
                    // sort by proximity to camera, then get rid of furthest away lights
                    std::sort(lightList.begin(), lightList.end(), sortLights);
                    while (lightList.size() > maxLights)
                        lightList.pop_back();
                }
                stateset = mLightManager->getLightListStateSet(lightList, cv->getTraversalNumber(), cv->getCurrentRenderStage()->getInitialViewMatrix());
            }
            else
                stateset = mLightManager->getLightListStateSet(mLightList, cv->getTraversalNumber(), cv->getCurrentRenderStage()->getInitialViewMatrix());


            cv->pushStateSet(stateset);
            return true;
        }
        return false;
    }

}
