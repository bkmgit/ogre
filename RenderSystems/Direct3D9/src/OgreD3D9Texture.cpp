/*
-----------------------------------------------------------------------------
This source file is part of OGRE
    (Object-oriented Graphics Rendering Engine)
For the latest info, see http://www.ogre3d.org/

Copyright (c) 2000-2014 Torus Knot Software Ltd

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------
*/
#include "OgreD3D9Texture.h"
#include "OgreD3D9HardwarePixelBuffer.h"
#include "OgreException.h"
#include "OgreLogManager.h"
#include "OgreStringConverter.h"
#include "OgreBitwise.h"
#include "OgreD3D9Mappings.h"
#include "OgreD3D9RenderSystem.h"
#include "OgreD3D9TextureManager.h"
#include "OgreRoot.h"
#include "OgreD3D9Device.h"
#include "OgreD3D9DeviceManager.h"
#include "OgreD3D9ResourceManager.h"
#include "OgreD3D9DepthBuffer.h"

#include <d3dx9.h>

namespace Ogre 
{
    /****************************************************************************************/
    D3D9Texture::D3D9Texture(ResourceManager* creator, const String& name, 
        ResourceHandle handle, const String& group, bool isManual, 
        ManualResourceLoader* loader)
        :Texture(creator, name, handle, group, isManual, loader),              
        mD3DPool(D3D9RenderSystem::isDirectX9Ex() ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED),
        mDynamicTextures(false),
        mHwGammaReadSupported(false),
        mHwGammaWriteSupported(false),  
        mFSAAType(D3DMULTISAMPLE_NONE),
        mFSAAQuality(0)
    {
       
    }
    /****************************************************************************************/
    D3D9Texture::~D3D9Texture()
    {   
        D3D9_DEVICE_ACCESS_CRITICAL_SECTION
        
        // have to call this here reather than in Resource destructor
        // since calling virtual methods in base destructors causes crash
        unload();

        // Free memory allocated per device.
        DeviceToTextureResourcesIterator it = mMapDeviceToTextureResources.begin();
        while (it != mMapDeviceToTextureResources.end())
        {
            TextureResources* textureResource = it->second;
            
            OGRE_DELETE_T(textureResource, TextureResources, MEMCATEGORY_RENDERSYS);
            ++it;
        }       
        mMapDeviceToTextureResources.clear();      
    }
    /****************************************************************************************/
    void D3D9Texture::copyToTexture(TexturePtr& target)
    {
        // check if this & target are the same format and type
        // blitting from or to cube textures is not supported yet
        if (target->getUsage() != getUsage() ||
            target->getTextureType() != getTextureType())
        {
            OGRE_EXCEPT( Exception::ERR_INVALIDPARAMS, 
                    "Src. and dest. textures must be of same type and must have the same usage !!!", 
                    "D3D9Texture::copyToTexture" );
        }

        HRESULT hr;
        D3D9Texture *other;
        // get the target
        other = reinterpret_cast< D3D9Texture * >( target.get() );
        // target rectangle (whole surface)
        RECT dstRC = {0, 0, static_cast<LONG>(other->getWidth()), static_cast<LONG>(other->getHeight())};

        DeviceToTextureResourcesIterator it = mMapDeviceToTextureResources.begin();
        while (it != mMapDeviceToTextureResources.end())
        {
            TextureResources* srcTextureResource = it->second;
            TextureResources* dstTextureResource = other->getTextureResources(it->first);

    
            // do it plain for normal texture
            if (getTextureType() == TEX_TYPE_2D &&
                srcTextureResource->pNormTex && 
                dstTextureResource->pNormTex)
            {           
                // get our source surface
                IDirect3DSurface9 *pSrcSurface = 0;
                if( FAILED( hr = srcTextureResource->pNormTex->GetSurfaceLevel(0, &pSrcSurface) ) )
                {
                    String msg = DXGetErrorDescription(hr);
                    OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, "Couldn't blit : " + msg, "D3D9Texture::copyToTexture" );
                }

                // get our target surface
                IDirect3DSurface9 *pDstSurface = 0;         
                if( FAILED( hr = dstTextureResource->pNormTex->GetSurfaceLevel(0, &pDstSurface) ) )
                {
                    String msg = DXGetErrorDescription(hr);
                    SAFE_RELEASE(pSrcSurface);
                    OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, "Couldn't blit : " + msg, "D3D9Texture::copyToTexture" );
                }

                // do the blit, it's called StretchRect in D3D9 :)
                if( FAILED( hr = it->first->StretchRect( pSrcSurface, NULL, pDstSurface, &dstRC, D3DTEXF_NONE) ) )
                {
                    String msg = DXGetErrorDescription(hr);
                    SAFE_RELEASE(pSrcSurface);
                    SAFE_RELEASE(pDstSurface);
                    OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, "Couldn't blit : " + msg, "D3D9Texture::copyToTexture" );
                }

                // release temp. surfaces
                SAFE_RELEASE(pSrcSurface);
                SAFE_RELEASE(pDstSurface);
            }
            else if (getTextureType() == TEX_TYPE_CUBE_MAP &&
                srcTextureResource->pCubeTex && 
                dstTextureResource->pCubeTex)
            {               
                // blit to 6 cube faces
                for (size_t face = 0; face < 6; face++)
                {
                    // get our source surface
                    IDirect3DSurface9 *pSrcSurface = 0;
                    if( FAILED( hr =srcTextureResource->pCubeTex->GetCubeMapSurface((D3DCUBEMAP_FACES)face, 0, &pSrcSurface) ) )
                    {
                        String msg = DXGetErrorDescription(hr);
                        OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, "Couldn't blit : " + msg, "D3D9Texture::copyToTexture" );
                    }

                    // get our target surface
                    IDirect3DSurface9 *pDstSurface = 0;
                    if( FAILED( hr = dstTextureResource->pCubeTex->GetCubeMapSurface((D3DCUBEMAP_FACES)face, 0, &pDstSurface) ) )
                    {
                        String msg = DXGetErrorDescription(hr);
                        SAFE_RELEASE(pSrcSurface);
                        OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, "Couldn't blit : " + msg, "D3D9Texture::copyToTexture" );
                    }

                    // do the blit, it's called StretchRect in D3D9 :)
                    if( FAILED( hr = it->first->StretchRect( pSrcSurface, NULL, pDstSurface, &dstRC, D3DTEXF_NONE) ) )
                    {
                        String msg = DXGetErrorDescription(hr);
                        SAFE_RELEASE(pSrcSurface);
                        SAFE_RELEASE(pDstSurface);
                        OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, "Couldn't blit : " + msg, "D3D9Texture::copyToTexture" );
                    }

                    // release temp. surfaces
                    SAFE_RELEASE(pSrcSurface);
                    SAFE_RELEASE(pDstSurface);
                }
            }
            else
            {
                OGRE_EXCEPT( Exception::ERR_NOT_IMPLEMENTED, 
                    "Copy to texture is implemented only for 2D and cube textures !!!", 
                    "D3D9Texture::copyToTexture" );
            }

            ++it;
        }       
    }
    /****************************************************************************************/
    void D3D9Texture::loadImpl()
    {
        if (!mInternalResourcesCreated)
        {
            // NB: Need to initialise pool to some value other than D3DPOOL_DEFAULT,
            // otherwise, if the texture loading failed, it might re-create as empty
            // texture when device lost/restore. The actual pool will determine later.
            //
            // In directX9Ex this is not the case there is no managed pool and no device loss
            mD3DPool = D3D9RenderSystem::isDirectX9Ex() ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED;
        }

        Texture::loadImpl();
    }

    /****************************************************************************************/
    void D3D9Texture::freeInternalResourcesImpl()
    {
        D3D9_DEVICE_ACCESS_CRITICAL_SECTION

        DeviceToTextureResourcesIterator it = mMapDeviceToTextureResources.begin();

        while (it != mMapDeviceToTextureResources.end())
        {
            TextureResources* textureResource = it->second;

            freeTextureResources(it->first, textureResource);           
            ++it;
        }                       
    }

    /****************************************************************************************/
    D3D9Texture::TextureResources* D3D9Texture::getTextureResources(IDirect3DDevice9* d3d9Device)
    {       
        DeviceToTextureResourcesIterator it = mMapDeviceToTextureResources.find(d3d9Device);

        if (it == mMapDeviceToTextureResources.end())       
            return NULL;        
        
        return it->second;
    }

    /****************************************************************************************/
    D3D9Texture::TextureResources* D3D9Texture::allocateTextureResources(IDirect3DDevice9* d3d9Device)
    {
        assert(mMapDeviceToTextureResources.find(d3d9Device) == mMapDeviceToTextureResources.end());

        TextureResources* textureResources = OGRE_NEW_T(TextureResources, MEMCATEGORY_RENDERSYS);

        textureResources->pNormTex      = NULL;
        textureResources->pCubeTex      = NULL;
        textureResources->pVolumeTex    = NULL;
        textureResources->pBaseTex      = NULL;
        textureResources->pFSAASurface  = NULL;

        mMapDeviceToTextureResources[d3d9Device] = textureResources;

        return textureResources;
    }

    /****************************************************************************************/
    void D3D9Texture::createTextureResources(IDirect3DDevice9* d3d9Device)
    {               
        D3D9_DEVICE_ACCESS_CRITICAL_SECTION
        
        if (isManuallyLoaded())
        {
            preLoadImpl();

            // create the internal resources.
            createInternalResourcesImpl(d3d9Device);

            // Load from manual loader
            if (mLoader != NULL)
            {
                mLoader->loadResource(this);
            }           
            postLoadImpl();
        }
        else
        {           
            prepareImpl();      

            preLoadImpl();

            loadImpl();

            postLoadImpl();         
        }       
    }

    /****************************************************************************************/
    void D3D9Texture::freeTextureResources(IDirect3DDevice9* d3d9Device, D3D9Texture::TextureResources* textureResources)
    {       
        D3D9_DEVICE_ACCESS_CRITICAL_SECTION

        // Release surfaces from each mip level.
        for(unsigned int i = 0; i < mSurfaceList.size(); ++i)
        {
            D3D9HardwarePixelBuffer* pixelBuffer = static_cast<D3D9HardwarePixelBuffer*>(mSurfaceList[i].get());

            pixelBuffer->releaseSurfaces(d3d9Device);           
        }
        
        // Release the rest of the resources.
        SAFE_RELEASE(textureResources->pBaseTex);
        SAFE_RELEASE(textureResources->pNormTex);
        SAFE_RELEASE(textureResources->pCubeTex);
        SAFE_RELEASE(textureResources->pVolumeTex);
        SAFE_RELEASE(textureResources->pFSAASurface);
    }

    /****************************************************************************************/
    size_t D3D9Texture::calculateSize(void) const
    {
        size_t instanceSize = getNumFaces() * PixelUtil::getMemorySize(mWidth, mHeight, mDepth, mFormat);

        return instanceSize * mMapDeviceToTextureResources.size();
    }

    /****************************************************************************************/
    void D3D9Texture::determinePool()
    {
        if (useDefaultPool())
        {
            mD3DPool = D3DPOOL_DEFAULT;
        }
        else
        {
            mD3DPool = D3DPOOL_MANAGED;
        }

    }
    /****************************************************************************************/
    void D3D9Texture::createInternalResourcesImpl(void)
    {
        D3D9_DEVICE_ACCESS_CRITICAL_SECTION

        for (uint i = 0; i < D3D9RenderSystem::getResourceCreationDeviceCount(); ++i)
        {
            IDirect3DDevice9* d3d9Device = D3D9RenderSystem::getResourceCreationDevice(i);

            createInternalResourcesImpl(d3d9Device);
        }
    }

    /****************************************************************************************/
    void D3D9Texture::createInternalResourcesImpl(IDirect3DDevice9* d3d9Device)
    {       
        TextureResources* textureResources;         

        // Check if resources already exist.
        textureResources = getTextureResources(d3d9Device);
        if (textureResources != NULL && textureResources->pBaseTex != NULL)
            return;
        

        // If mSrcWidth and mSrcHeight are zero, the requested extents have probably been set
        // through setWidth and setHeight, which set mWidth and mHeight. Take those values.
        if(mSrcWidth == 0 || mSrcHeight == 0) {
            mSrcWidth = mWidth;
            mSrcHeight = mHeight;
        }

        if(mUsage & TU_AUTOMIPMAP && D3D9RenderSystem::isDirectX9Ex())
        {
            mUsage |= TU_DYNAMIC;
        }

        if(!PixelUtil::isDepth(mFormat))
            mFormat = TextureManager::getSingleton().getNativeFormat(mTextureType, mFormat, mUsage);

        // load based on tex.type
        switch (getTextureType())
        {
        case TEX_TYPE_1D:
        case TEX_TYPE_2D:
            _createNormTex(d3d9Device);
            break;
        case TEX_TYPE_CUBE_MAP:
            _createCubeTex(d3d9Device);
            break;
        case TEX_TYPE_3D:
            _createVolumeTex(d3d9Device);
            break;
        default:
            freeInternalResources();
            OGRE_EXCEPT( Exception::ERR_INTERNAL_ERROR, "Unknown texture type", "D3D9Texture::createInternalResources" );
        }
    }

    /****************************************************************************************/
    void D3D9Texture::_createNormTex(IDirect3DDevice9* d3d9Device)
    {
        // we must have those defined here
        assert(mSrcWidth > 0 || mSrcHeight > 0);

        // determine which D3D9 pixel format we'll use
        HRESULT hr;
        D3DFORMAT d3dPF = D3D9Mappings::_getPF(mFormat);

        // Use D3DX to help us create the texture, this way it can adjust any relevant sizes
        DWORD usage = (mUsage & TU_RENDERTARGET) ? D3DUSAGE_RENDERTARGET : 0;
        UINT numMips = (mNumRequestedMipmaps == MIP_UNLIMITED) ? 0 : mNumRequestedMipmaps + 1;
        // Check dynamic textures
        if (mUsage & TU_DYNAMIC)
        {
            if (_canUseDynamicTextures(d3d9Device, usage, D3DRTYPE_TEXTURE, d3dPF))
            {
                usage |= D3DUSAGE_DYNAMIC;
                mDynamicTextures = true;
            }
            else
            {
                mDynamicTextures = false;
            }
        }
        // Check sRGB support
        if (mHwGamma)
        {
            mHwGammaReadSupported = _canUseHardwareGammaCorrection(d3d9Device, usage, D3DRTYPE_TEXTURE, d3dPF, false);
            if (mUsage & TU_RENDERTARGET)
                mHwGammaWriteSupported = _canUseHardwareGammaCorrection(d3d9Device, usage, D3DRTYPE_TEXTURE, d3dPF, true);
        }
        // Check FSAA level
        if (mUsage & TU_RENDERTARGET)
        {
            D3D9RenderSystem* rsys = static_cast<D3D9RenderSystem*>(Root::getSingleton().getRenderSystem());
            rsys->determineFSAASettings(d3d9Device, mFSAA, mFSAAHint, d3dPF, false, 
                &mFSAAType, &mFSAAQuality);
        }
        else
        {
            mFSAAType = D3DMULTISAMPLE_NONE;
            mFSAAQuality = 0;
        }

        D3D9Device* device = D3D9RenderSystem::getDeviceManager()->getDeviceFromD3D9Device(d3d9Device);
        const D3DCAPS9& rkCurCaps = device->getD3D9DeviceCaps();            


        // check if mip maps are supported on hardware
        mMipmapsHardwareGenerated = false;
        if (rkCurCaps.TextureCaps & D3DPTEXTURECAPS_MIPMAP)
        {
            if (mUsage & TU_AUTOMIPMAP && mNumRequestedMipmaps != 0)
            {
                // use auto.gen. if available, and if desired
                mMipmapsHardwareGenerated = _canAutoGenMipmaps(d3d9Device, usage, D3DRTYPE_TEXTURE, d3dPF);
                if (mMipmapsHardwareGenerated)
                {
                    usage |= D3DUSAGE_AUTOGENMIPMAP;
                    numMips = 0;
                }
            }
        }
        else
        {
            // no mip map support for this kind of textures :(
            mNumMipmaps = 0;
            numMips = 1;
        }

        // derive the pool to use
        determinePool();

        TextureResources* textureResources;         
    
        // Get or create new texture resources structure.
        textureResources = getTextureResources(d3d9Device);
        if (textureResources != NULL)
            freeTextureResources(d3d9Device, textureResources);
        else
            textureResources = allocateTextureResources(d3d9Device);

        if(PixelUtil::isDepth(mFormat))
        {
            usage = D3DUSAGE_DEPTHSTENCIL;
            mD3DPool = D3DPOOL_DEFAULT;
            // we cannot resolve depth on D3D9
            mFSAAType = D3DMULTISAMPLE_NONE;
            mFSAA = 0;
        }

        // create the texture
        hr = d3d9Device->CreateTexture(
                static_cast<UINT>(mSrcWidth),           // width
                static_cast<UINT>(mSrcHeight),          // height
                numMips,                                // number of mip map levels
                usage,                                  // usage
                d3dPF,                                  // pixel format
                mD3DPool,
                &textureResources->pNormTex, NULL);
        // check result and except if failed
        if (FAILED(hr))
        {
            freeInternalResources();
            OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, "Error creating texture: " + String(DXGetErrorDescription(hr)), 
                "D3D9Texture::_createNormTex" );
        }
        
        // set the base texture we'll use in the render system
        hr = textureResources->pNormTex->QueryInterface(IID_IDirect3DBaseTexture9, (void **)&textureResources->pBaseTex);
        if (FAILED(hr))
        {
            freeInternalResources();
            OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, "Can't get base texture: " + String(DXGetErrorDescription(hr)), 
                "D3D9Texture::_createNormTex" );
        }
        
        // set final tex. attributes from tex. description
        // they may differ from the source image !!!
        D3DSURFACE_DESC desc;
        hr = textureResources->pNormTex->GetLevelDesc(0, &desc);
        if (FAILED(hr))
        {
            freeInternalResources();
            OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, "Can't get texture description: " + String(DXGetErrorDescription(hr)), 
                "D3D9Texture::_createNormTex" );
        }

        if (mFSAAType)
        {
            // create AA surface
            HRESULT hr = d3d9Device->CreateRenderTarget(desc.Width, desc.Height, d3dPF, 
                mFSAAType, 
                mFSAAQuality,
                FALSE, // not lockable
                &textureResources->pFSAASurface, NULL);

            if (FAILED(hr))
            {
                OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                    "Unable to create AA render target: " + String(DXGetErrorDescription(hr)), 
                    "D3D9Texture::_createNormTex");
            }

        }

        _setFinalAttributes(d3d9Device, textureResources, 
            desc.Width, desc.Height, 1, D3D9Mappings::_getPF(desc.Format));
        
        // Set best filter type
        if(mMipmapsHardwareGenerated)
        {
            hr = textureResources->pBaseTex->SetAutoGenFilterType(_getBestFilterMethod(d3d9Device));
            if(FAILED(hr))
            {
                OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, "Could not set best autogen filter type: "  + String(DXGetErrorDescription(hr)), 
                    "D3D9Texture::_createNormTex" );
            }
        }

    }
    /****************************************************************************************/
    void D3D9Texture::_createCubeTex(IDirect3DDevice9* d3d9Device)
    {
        // we must have those defined here
        assert(mSrcWidth > 0 || mSrcHeight > 0);

        // determine which D3D9 pixel format we'll use
        HRESULT hr;
        D3DFORMAT d3dPF = D3D9Mappings::_getPF(mFormat);

        // Use D3DX to help us create the texture, this way it can adjust any relevant sizes
        DWORD usage = (mUsage & TU_RENDERTARGET) ? D3DUSAGE_RENDERTARGET : 0;
        UINT numMips = (mNumRequestedMipmaps == MIP_UNLIMITED) ? 0 : mNumRequestedMipmaps + 1;
        // Check dynamic textures
        if (mUsage & TU_DYNAMIC)
        {
            if (_canUseDynamicTextures(d3d9Device, usage, D3DRTYPE_CUBETEXTURE, d3dPF))
            {
                usage |= D3DUSAGE_DYNAMIC;
                mDynamicTextures = true;
            }
            else
            {
                mDynamicTextures = false;
            }
        }
        // Check sRGB support
        if (mHwGamma)
        {
            mHwGammaReadSupported = _canUseHardwareGammaCorrection(d3d9Device, usage, D3DRTYPE_CUBETEXTURE, d3dPF, false);
            if (mUsage & TU_RENDERTARGET)
                mHwGammaWriteSupported = _canUseHardwareGammaCorrection(d3d9Device, usage, D3DRTYPE_CUBETEXTURE, d3dPF, true);
        }
        // Check FSAA level
        if (mUsage & TU_RENDERTARGET)
        {
            D3D9RenderSystem* rsys = static_cast<D3D9RenderSystem*>(Root::getSingleton().getRenderSystem());
            rsys->determineFSAASettings(d3d9Device, mFSAA, mFSAAHint, d3dPF, false, 
                &mFSAAType, &mFSAAQuality);
        }
        else
        {
            mFSAAType = D3DMULTISAMPLE_NONE;
            mFSAAQuality = 0;
        }

        D3D9Device* device = D3D9RenderSystem::getDeviceManager()->getDeviceFromD3D9Device(d3d9Device);
        const D3DCAPS9& rkCurCaps = device->getD3D9DeviceCaps();            
        
        // check if mip map cube textures are supported
        mMipmapsHardwareGenerated = false;
        if (rkCurCaps.TextureCaps & D3DPTEXTURECAPS_MIPCUBEMAP)
        {
            if (mUsage & TU_AUTOMIPMAP && mNumRequestedMipmaps != 0)
            {
                // use auto.gen. if available
                mMipmapsHardwareGenerated = _canAutoGenMipmaps(d3d9Device, usage, D3DRTYPE_CUBETEXTURE, d3dPF);
                if (mMipmapsHardwareGenerated)
                {
                    usage |= D3DUSAGE_AUTOGENMIPMAP;
                }
            }
        }
        else
        {
            // no mip map support for this kind of textures :(
            mNumMipmaps = 0;
            numMips = 1;
        }

        // derive the pool to use
        determinePool();
        TextureResources* textureResources;         

        // Get or create new texture resources structure.
        textureResources = getTextureResources(d3d9Device);
        if (textureResources != NULL)
            freeTextureResources(d3d9Device, textureResources);
        else
            textureResources = allocateTextureResources(d3d9Device);


        // create the texture
        hr = d3d9Device->CreateCubeTexture(
                static_cast<UINT>(mSrcWidth),               // dimension
                numMips,                                    // number of mip map levels
                usage,                                      // usage
                d3dPF,                                      // pixel format
                mD3DPool,
                &textureResources->pCubeTex, NULL);
        // check result and except if failed
        if (FAILED(hr))
        {
            freeInternalResources();
            OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, "Error creating texture: " + String(DXGetErrorDescription(hr)), 
                "D3D9Texture::_createCubeTex" );
        }

        // set the base texture we'll use in the render system
        hr = textureResources->pCubeTex->QueryInterface(IID_IDirect3DBaseTexture9, (void **)&textureResources->pBaseTex);
        if (FAILED(hr))
        {
            freeInternalResources();
            OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, "Can't get base texture: " + String(DXGetErrorDescription(hr)), 
                "D3D9Texture::_createCubeTex" );
        }
        
        // set final tex. attributes from tex. description
        // they may differ from the source image !!!
        D3DSURFACE_DESC desc;
        hr = textureResources->pCubeTex->GetLevelDesc(0, &desc);
        if (FAILED(hr))
        {
            freeInternalResources();
            OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, "Can't get texture description: " + String(DXGetErrorDescription(hr)), 
                "D3D9Texture::_createCubeTex" );
        }

        if (mFSAAType)
        {
            // create AA surface
            HRESULT hr = d3d9Device->CreateRenderTarget(desc.Width, desc.Height, d3dPF, 
                mFSAAType, 
                mFSAAQuality,
                FALSE, // not lockable
                &textureResources->pFSAASurface, NULL);

            if (FAILED(hr))
            {
                OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                    "Unable to create AA render target: " + String(DXGetErrorDescription(hr)), 
                    "D3D9Texture::_createCubeTex");
            }
        }

        _setFinalAttributes(d3d9Device, textureResources, 
            desc.Width, desc.Height, 1, D3D9Mappings::_getPF(desc.Format));

        // Set best filter type
        if(mMipmapsHardwareGenerated)
        {
            hr = textureResources->pBaseTex->SetAutoGenFilterType(_getBestFilterMethod(d3d9Device));
            if(FAILED(hr))
            {
                OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, "Could not set best autogen filter type: " + String(DXGetErrorDescription(hr)), 
                    "D3D9Texture::_createCubeTex" );
            }
        }

    }
    /****************************************************************************************/
    void D3D9Texture::_createVolumeTex(IDirect3DDevice9* d3d9Device)
    {
        // we must have those defined here
        assert(mWidth > 0 && mHeight > 0 && mDepth>0);

        if (mUsage & TU_RENDERTARGET)
        {
            OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, "D3D9 Volume texture can not be created as render target !!", 
                "D3D9Texture::_createVolumeTex" );
        }

        // determine which D3D9 pixel format we'll use
        HRESULT hr;
        D3DFORMAT d3dPF = D3D9Mappings::_getPF(mFormat);

        // Use D3DX to help us create the texture, this way it can adjust any relevant sizes
        DWORD usage = (mUsage & TU_RENDERTARGET) ? D3DUSAGE_RENDERTARGET : 0;
        UINT numMips = (mNumRequestedMipmaps == MIP_UNLIMITED) ? 0 : mNumRequestedMipmaps + 1;
        // Check dynamic textures
        if (mUsage & TU_DYNAMIC)
        {
            if (_canUseDynamicTextures(d3d9Device, usage, D3DRTYPE_VOLUMETEXTURE, d3dPF))
            {
                usage |= D3DUSAGE_DYNAMIC;
                mDynamicTextures = true;
            }
            else
            {
                mDynamicTextures = false;
            }
        }
        // Check sRGB support
        if (mHwGamma)
        {
            mHwGammaReadSupported = _canUseHardwareGammaCorrection(d3d9Device, usage, D3DRTYPE_VOLUMETEXTURE, d3dPF, false);
            if (mUsage & TU_RENDERTARGET)
                mHwGammaWriteSupported = _canUseHardwareGammaCorrection(d3d9Device, usage, D3DRTYPE_VOLUMETEXTURE, d3dPF, true);
        }

        D3D9Device* device = D3D9RenderSystem::getDeviceManager()->getDeviceFromD3D9Device(d3d9Device);
        const D3DCAPS9& rkCurCaps = device->getD3D9DeviceCaps();            


        // check if mip map volume textures are supported
        mMipmapsHardwareGenerated = false;
        if (rkCurCaps.TextureCaps & D3DPTEXTURECAPS_MIPVOLUMEMAP)
        {
            if (mUsage & TU_AUTOMIPMAP && mNumRequestedMipmaps != 0)
            {
                // use auto.gen. if available
                mMipmapsHardwareGenerated = _canAutoGenMipmaps(d3d9Device, usage, D3DRTYPE_VOLUMETEXTURE, d3dPF);
                if (mMipmapsHardwareGenerated)
                {
                    usage |= D3DUSAGE_AUTOGENMIPMAP;
                    numMips = 0;
                }
            }
        }
        else
        {
            // no mip map support for this kind of textures :(
            mNumMipmaps = 0;
            numMips = 1;
        }

        // derive the pool to use
        determinePool();
        TextureResources* textureResources;         

        // Get or create new texture resources structure.
        textureResources = getTextureResources(d3d9Device);
        if (textureResources != NULL)
            freeTextureResources(d3d9Device, textureResources);
        else
            textureResources = allocateTextureResources(d3d9Device);


        // create the texture
        hr = d3d9Device->CreateVolumeTexture(
                static_cast<UINT>(mWidth),                  // dimension
                static_cast<UINT>(mHeight),
                static_cast<UINT>(mDepth),
                numMips,                                    // number of mip map levels
                usage,                                      // usage
                d3dPF,                                      // pixel format
                mD3DPool,
                &textureResources->pVolumeTex, NULL);
        // check result and except if failed
        if (FAILED(hr))
        {
            freeInternalResources();
            OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, "Error creating texture: " + String(DXGetErrorDescription(hr)), 
                "D3D9Texture::_createVolumeTex" );
        }

        // set the base texture we'll use in the render system
        hr = textureResources->pVolumeTex->QueryInterface(IID_IDirect3DBaseTexture9, (void **)&textureResources->pBaseTex);
        if (FAILED(hr))
        {
            freeInternalResources();
            OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, "Can't get base texture: " + String(DXGetErrorDescription(hr)), 
                "D3D9Texture::_createVolumeTex" );
        }
        
        // set final tex. attributes from tex. description
        // they may differ from the source image !!!
        D3DVOLUME_DESC desc;
        hr = textureResources->pVolumeTex->GetLevelDesc(0, &desc);
        if (FAILED(hr))
        {
            freeInternalResources();
            OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, "Can't get texture description: " + String(DXGetErrorDescription(hr)), 
                "D3D9Texture::_createVolumeTex" );
        }
        _setFinalAttributes(d3d9Device, textureResources,
            desc.Width, desc.Height, desc.Depth, D3D9Mappings::_getPF(desc.Format));
        
        // Set best filter type
        if(mMipmapsHardwareGenerated)
        {
            hr = textureResources->pBaseTex->SetAutoGenFilterType(_getBestFilterMethod(d3d9Device));
            if(FAILED(hr))
            {
                OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, "Could not set best autogen filter type: " + String(DXGetErrorDescription(hr)), 
                    "D3D9Texture::_createCubeTex" );
            }
        }
    }

    /****************************************************************************************/
    void D3D9Texture::_setFinalAttributes(IDirect3DDevice9* d3d9Device, 
        TextureResources* textureResources,
        unsigned long width, unsigned long height, 
        unsigned long depth, PixelFormat format)
    { 
        // set target texture attributes
        mHeight = height; 
        mWidth = width; 
        mDepth = depth;
        mFormat = format; 

        // Update size (the final size, including temp space because in consumed memory)
        // this is needed in Resource class
        mSize = calculateSize();

        // say to the world what we are doing
        if (mWidth != mSrcWidth ||
            mHeight != mSrcHeight)
        {
            LogManager::getSingleton().logMessage("D3D9 : ***** Dimensions altered by the render system");
            LogManager::getSingleton().logMessage("D3D9 : ***** Source image dimensions : " + StringConverter::toString(mSrcWidth) + "x" + StringConverter::toString(mSrcHeight));
            LogManager::getSingleton().logMessage("D3D9 : ***** Texture dimensions : " + StringConverter::toString(mWidth) + "x" + StringConverter::toString(mHeight));
        }
        
        // Create list of subsurfaces for getBuffer()
        _createSurfaceList(d3d9Device, textureResources);
    }
    /****************************************************************************************/
    void D3D9Texture::_setSrcAttributes(unsigned long width, unsigned long height, 
        unsigned long depth, PixelFormat format)
    { 
        // set source image attributes
        mSrcWidth = width; 
        mSrcHeight = height; 
        mSrcDepth = depth;
        mSrcFormat = format;
        // say to the world what we are doing
        if (!TextureManager::getSingleton().getVerbose()) return;
        switch (getTextureType())
        {
        case TEX_TYPE_1D:
            if (mUsage & TU_RENDERTARGET)
                LogManager::getSingleton().logMessage("D3D9 : Creating 1D RenderTarget, name : '" + getName() + "' with " + StringConverter::toString(mNumMipmaps) + " mip map levels");
            else
                LogManager::getSingleton().logMessage("D3D9 : Loading 1D Texture, image name : '" + getName() + "' with " + StringConverter::toString(mNumMipmaps) + " mip map levels");
            break;
        case TEX_TYPE_2D:
            if (mUsage & TU_RENDERTARGET)
                LogManager::getSingleton().logMessage("D3D9 : Creating 2D RenderTarget, name : '" + getName() + "' with " + StringConverter::toString(mNumMipmaps) + " mip map levels");
            else
                LogManager::getSingleton().logMessage("D3D9 : Loading 2D Texture, image name : '" + getName() + "' with " + StringConverter::toString(mNumMipmaps) + " mip map levels");
            break;
        case TEX_TYPE_3D:
            if (mUsage & TU_RENDERTARGET)
                LogManager::getSingleton().logMessage("D3D9 : Creating 3D RenderTarget, name : '" + getName() + "' with " + StringConverter::toString(mNumMipmaps) + " mip map levels");
            else
                LogManager::getSingleton().logMessage("D3D9 : Loading 3D Texture, image name : '" + getName() + "' with " + StringConverter::toString(mNumMipmaps) + " mip map levels");
            break;
        case TEX_TYPE_CUBE_MAP:
            if (mUsage & TU_RENDERTARGET)
                LogManager::getSingleton().logMessage("D3D9 : Creating Cube map RenderTarget, name : '" + getName() + "' with " + StringConverter::toString(mNumMipmaps) + " mip map levels");
            else
                LogManager::getSingleton().logMessage("D3D9 : Loading Cube Texture, base image name : '" + getName() + "' with " + StringConverter::toString(mNumMipmaps) + " mip map levels");
            break;
        default:
            freeInternalResources();
            OGRE_EXCEPT( Exception::ERR_INTERNAL_ERROR, "Unknown texture type", "D3D9Texture::_setSrcAttributes" );
        }
    }
    /****************************************************************************************/
    D3DTEXTUREFILTERTYPE D3D9Texture::_getBestFilterMethod(IDirect3DDevice9* d3d9Device)
    {
        D3D9Device* device = D3D9RenderSystem::getDeviceManager()->getDeviceFromD3D9Device(d3d9Device);
        const D3DCAPS9& rkCurCaps = device->getD3D9DeviceCaps();            
        
        
        DWORD filterCaps = 0;
        // Minification filter is used for mipmap generation
        // Pick the best one supported for this tex type
        switch (getTextureType())
        {
        case TEX_TYPE_1D:       // Same as 2D
        case TEX_TYPE_2D:       filterCaps = rkCurCaps.TextureFilterCaps;   break;
        case TEX_TYPE_3D:       filterCaps = rkCurCaps.VolumeTextureFilterCaps; break;
        case TEX_TYPE_CUBE_MAP: filterCaps = rkCurCaps.CubeTextureFilterCaps;   break;
        }
        if(filterCaps & D3DPTFILTERCAPS_MINFGAUSSIANQUAD)
            return D3DTEXF_GAUSSIANQUAD;
        
        if(filterCaps & D3DPTFILTERCAPS_MINFPYRAMIDALQUAD)
            return D3DTEXF_PYRAMIDALQUAD;
        
        if(filterCaps & D3DPTFILTERCAPS_MINFANISOTROPIC)
            return D3DTEXF_ANISOTROPIC;
        
        if(filterCaps & D3DPTFILTERCAPS_MINFLINEAR)
            return D3DTEXF_LINEAR;
        
        if(filterCaps & D3DPTFILTERCAPS_MINFPOINT)
            return D3DTEXF_POINT;
        
        return D3DTEXF_POINT;
    }
    /****************************************************************************************/
    bool D3D9Texture::_canUseDynamicTextures(IDirect3DDevice9* d3d9Device, 
        DWORD srcUsage, 
        D3DRESOURCETYPE srcType, 
        D3DFORMAT srcFormat)
    {       
        HRESULT hr;
        IDirect3D9* pD3D = NULL;

        hr = d3d9Device->GetDirect3D(&pD3D);
        if (FAILED(hr))
        {
            OGRE_EXCEPT( Exception::ERR_INVALIDPARAMS, 
                "GetDirect3D failed !!!",               
                "D3D9Texture::_canUseDynamicTextures" );
        }
        if (pD3D != NULL)
            pD3D->Release();
    
        D3D9Device* device = D3D9RenderSystem::getDeviceManager()->getDeviceFromD3D9Device(d3d9Device);
        const D3DCAPS9& rkCurCaps = device->getD3D9DeviceCaps();                        
        D3DFORMAT eBackBufferFormat = device->getBackBufferFormat();

        
        // Check for dynamic texture support
        
        // check for auto gen. mip maps support
        hr = pD3D->CheckDeviceFormat(
            rkCurCaps.AdapterOrdinal, 
            rkCurCaps.DeviceType, 
            eBackBufferFormat, 
            srcUsage | D3DUSAGE_DYNAMIC,
            srcType,
            srcFormat);

        if (hr == D3D_OK)
            return true;
        else
            return false;
    }
    /****************************************************************************************/
    bool D3D9Texture::_canUseHardwareGammaCorrection(IDirect3DDevice9* d3d9Device,
        DWORD srcUsage, 
        D3DRESOURCETYPE srcType, D3DFORMAT srcFormat, bool forwriting)
    {
        HRESULT hr;
        IDirect3D9* pD3D = NULL;

        hr = d3d9Device->GetDirect3D(&pD3D);
        if (FAILED(hr))
        {
            OGRE_EXCEPT( Exception::ERR_INVALIDPARAMS, 
                "GetDirect3D failed !!!", 
                "D3D9Texture::_canUseDynamicTextures" );
        }
        if (pD3D != NULL)
            pD3D->Release();

        D3D9Device* device = D3D9RenderSystem::getDeviceManager()->getDeviceFromD3D9Device(d3d9Device);
        const D3DCAPS9& rkCurCaps = device->getD3D9DeviceCaps();                        
        D3DFORMAT eBackBufferFormat = device->getBackBufferFormat();


        // Always check 'read' capability here
        // We will check 'write' capability only in the context of a render target
        if (forwriting)
            srcUsage |= D3DUSAGE_QUERY_SRGBWRITE;
        else
            srcUsage |= D3DUSAGE_QUERY_SRGBREAD;

        // Check for sRGB support       
        // check for auto gen. mip maps support
        hr = pD3D->CheckDeviceFormat(
            rkCurCaps.AdapterOrdinal, 
            rkCurCaps.DeviceType, 
            eBackBufferFormat, 
            srcUsage,
            srcType,
            srcFormat);
        if (hr == D3D_OK)
            return true;
        else
            return false;

    }
    /****************************************************************************************/
    bool D3D9Texture::_canAutoGenMipmaps(IDirect3DDevice9* d3d9Device, 
        DWORD srcUsage, D3DRESOURCETYPE srcType, D3DFORMAT srcFormat)
    {
        HRESULT hr;
        IDirect3D9* pD3D = NULL;

        hr = d3d9Device->GetDirect3D(&pD3D);
        if (FAILED(hr))
        {
            OGRE_EXCEPT( Exception::ERR_INVALIDPARAMS, 
                "GetDirect3D failed !!!", 
                "D3D9Texture::_canUseDynamicTextures" );
        }
        if (pD3D != NULL)
            pD3D->Release();

        D3D9Device* device = D3D9RenderSystem::getDeviceManager()->getDeviceFromD3D9Device(d3d9Device);
        const D3DCAPS9& rkCurCaps = device->getD3D9DeviceCaps();                        
        D3DFORMAT eBackBufferFormat = device->getBackBufferFormat();

        if ((rkCurCaps.Caps2 & D3DCAPS2_CANAUTOGENMIPMAP) == 0)
            return false;

        // check for auto gen. mip maps support
        hr = pD3D->CheckDeviceFormat(
                rkCurCaps.AdapterOrdinal,
                rkCurCaps.DeviceType,
                eBackBufferFormat,
                srcUsage | D3DUSAGE_AUTOGENMIPMAP,
                srcType,
                srcFormat);
        // this HR could a SUCCES
        // but mip maps will not be generated
        return hr == D3D_OK;
    }
    /****************************************************************************************/
    // Macro to hide ugly cast
    #define GETLEVEL(face,mip) \
        static_cast<D3D9HardwarePixelBuffer*>(mSurfaceList[face*(mNumMipmaps+1)+mip].get())
    void D3D9Texture::_createSurfaceList(IDirect3DDevice9* d3d9Device, TextureResources* textureResources)
    {
        IDirect3DSurface9 *surface;
        IDirect3DVolume9 *volume;
        D3D9HardwarePixelBuffer *buffer;                
        size_t mip, face;

        
        assert(textureResources != NULL);
        assert(textureResources->pBaseTex);
        // Make sure number of mips is right
        mNumMipmaps = static_cast<uint32>(textureResources->pBaseTex->GetLevelCount() - 1);
        // Need to know static / dynamic
        unsigned int bufusage;
        if ((mUsage & TU_DYNAMIC) && mDynamicTextures)
        {
            bufusage = HardwareBuffer::HBU_DYNAMIC;
        }
        else
        {
            bufusage = HardwareBuffer::HBU_STATIC;
        }
        if (mUsage & TU_RENDERTARGET)
        {
            bufusage |= TU_RENDERTARGET;
        }
        
        uint surfaceCount  = static_cast<uint>((getNumFaces() * (mNumMipmaps + 1)));
        bool updateOldList = mSurfaceList.size() == surfaceCount;
        if(!updateOldList)
        {           
            // Create new list of surfaces  
            mSurfaceList.clear();
            for(size_t face=0; face<getNumFaces(); ++face)
            {
                for(size_t mip=0; mip<=mNumMipmaps; ++mip)
                {
                    buffer = OGRE_NEW D3D9HardwarePixelBuffer((HardwareBuffer::Usage)bufusage, this);
                    mSurfaceList.push_back(HardwarePixelBufferSharedPtr(buffer));
                }
            }
        }

        

        switch(getTextureType()) {
        case TEX_TYPE_2D:
        case TEX_TYPE_1D:
            assert(textureResources->pNormTex);
            // For all mipmaps, store surfaces as HardwarePixelBufferSharedPtr
            for(mip=0; mip<=mNumMipmaps; ++mip)
            {
                if(textureResources->pNormTex->GetSurfaceLevel(static_cast<UINT>(mip), &surface) != D3D_OK)
                    OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, "Get surface level failed",
                        "D3D9Texture::_createSurfaceList");

                D3D9HardwarePixelBuffer* currPixelBuffer = GETLEVEL(0, mip);
                                
                if (mip == 0 && mNumRequestedMipmaps != 0 && (mUsage & TU_AUTOMIPMAP))
                    currPixelBuffer->_setMipmapping(true, mMipmapsHardwareGenerated);

                currPixelBuffer->bind(d3d9Device, surface, textureResources->pFSAASurface,
                    mHwGammaWriteSupported, mFSAA, mName, textureResources->pBaseTex);

                // decrement reference count, the GetSurfaceLevel call increments this
                // this is safe because the pixel buffer keeps a reference as well
                surface->Release();         
            }
            
            break;
        case TEX_TYPE_CUBE_MAP:
            assert(textureResources->pCubeTex);
            // For all faces and mipmaps, store surfaces as HardwarePixelBufferSharedPtr
            for(face=0; face<6; ++face)
            {
                for(mip=0; mip<=mNumMipmaps; ++mip)
                {
                    if(textureResources->pCubeTex->GetCubeMapSurface((D3DCUBEMAP_FACES)face, static_cast<UINT>(mip), &surface) != D3D_OK)
                        OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, "Get cubemap surface failed",
                        "D3D9Texture::getBuffer");

                    D3D9HardwarePixelBuffer* currPixelBuffer = GETLEVEL(face, mip);
                    
                    
                    if (mip == 0 && mNumRequestedMipmaps != 0 && (mUsage & TU_AUTOMIPMAP))
                        currPixelBuffer->_setMipmapping(true, mMipmapsHardwareGenerated);

                    currPixelBuffer->bind(d3d9Device, surface, textureResources->pFSAASurface,
                        mHwGammaWriteSupported, mFSAA, mName, textureResources->pBaseTex);

                    // decrement reference count, the GetSurfaceLevel call increments this
                    // this is safe because the pixel buffer keeps a reference as well
                    surface->Release();             
                }               
            }
            break;
        case TEX_TYPE_3D:
            assert(textureResources->pVolumeTex);
            // For all mipmaps, store surfaces as HardwarePixelBufferSharedPtr
            for(mip=0; mip<=mNumMipmaps; ++mip)
            {
                if(textureResources->pVolumeTex->GetVolumeLevel(static_cast<UINT>(mip), &volume) != D3D_OK)
                    OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, "Get volume level failed",
                        "D3D9Texture::getBuffer");  
                        
                D3D9HardwarePixelBuffer* currPixelBuffer = GETLEVEL(0, mip);

                currPixelBuffer->bind(d3d9Device, volume, textureResources->pBaseTex);

                if (mip == 0 && mNumRequestedMipmaps != 0 && (mUsage & TU_AUTOMIPMAP))
                    currPixelBuffer->_setMipmapping(true, mMipmapsHardwareGenerated);

                // decrement reference count, the GetSurfaceLevel call increments this
                // this is safe because the pixel buffer keeps a reference as well
                volume->Release();
            }
            break;
        };                  
    }
    #undef GETLEVEL
    /****************************************************************************************/
    const HardwarePixelBufferSharedPtr& D3D9Texture::getBuffer(size_t face, size_t mipmap)
    {
        IDirect3DDevice9* d3d9Device = D3D9RenderSystem::getActiveD3D9Device();
        TextureResources* textureResources = getTextureResources(d3d9Device);
        if ((!textureResources || !textureResources->pBaseTex) && isLoaded())
        {
            // FIXME
            // createTextureResources(d3d9Device);
            // createTextureResources calls getBuffer again causing a stackoverflow
            // prefer an empty texture to crashing for now
            // to fix this we should probably use the notify mechanism instead of
            // recrating the texture in here..
            createInternalResourcesImpl(d3d9Device);
            textureResources = getTextureResources(d3d9Device);         
        }
        assert(textureResources != NULL);

        return Texture::getBuffer(face, mipmap);
    }
    //---------------------------------------------------------------------
    bool D3D9Texture::useDefaultPool()
    {
        // Determine D3D pool to use
        // Use managed unless we're a render target or user has asked for 
        // a dynamic texture, and device supports D3DUSAGE_DYNAMIC (because default pool
        // resources without the dynamic flag are not lockable)
        // or use if we are using directX9Ex as there is no managed pool under it.
        return (D3D9RenderSystem::isDirectX9Ex()) || (mUsage & TU_RENDERTARGET) || ((mUsage & TU_DYNAMIC) && mDynamicTextures);
    }
    
    //---------------------------------------------------------------------
    void D3D9Texture::notifyOnDeviceCreate(IDirect3DDevice9* d3d9Device) 
    {       
        D3D9_DEVICE_ACCESS_CRITICAL_SECTION

        if (D3D9RenderSystem::getResourceManager()->getCreationPolicy() == RCP_CREATE_ON_ALL_DEVICES)
        {
            try
            {
                createTextureResources(d3d9Device);
            }
            catch (...)
            {
                mLoadingState.store(LOADSTATE_UNLOADED);
                LogManager::getSingleton().stream(LML_WARNING)
                    << "Warning: Failed to restore texture " << getName() << " on DeviceCreate.";
            }
        }
    }

    //---------------------------------------------------------------------
    void D3D9Texture::notifyOnDeviceDestroy(IDirect3DDevice9* d3d9Device) 
    {               
        D3D9_DEVICE_ACCESS_CRITICAL_SECTION

        DeviceToTextureResourcesIterator it = mMapDeviceToTextureResources.find(d3d9Device);

        if (it != mMapDeviceToTextureResources.end())
        {           
            StringStream ss;

            ss << "D3D9 device: 0x[" << d3d9Device << "] destroy. Releasing D3D9 texture: " << mName;
            LogManager::getSingleton().logMessage(ss.str());

            TextureResources* textureResource = it->second;

            // Destroy surfaces from each mip level.
            for(unsigned int i = 0; i < mSurfaceList.size(); ++i)
            {
                D3D9HardwarePixelBuffer* pixelBuffer = static_cast<D3D9HardwarePixelBuffer*>(mSurfaceList[i].get());

                pixelBuffer->destroyBufferResources(d3d9Device);            
            }

            // Just free any internal resources, don't call unload() here
            // because we want the un-touched resource to keep its unloaded status
            // after device reset.
            freeTextureResources(d3d9Device, textureResource);

            OGRE_DELETE_T(textureResource, TextureResources, MEMCATEGORY_RENDERSYS);
            
            mMapDeviceToTextureResources.erase(it);

            LogManager::getSingleton().logMessage("Released D3D9 texture: " + mName);   
        }   
    }

    //---------------------------------------------------------------------
    void D3D9Texture::notifyOnDeviceLost(IDirect3DDevice9* d3d9Device) 
    {       
        D3D9_DEVICE_ACCESS_CRITICAL_SECTION

        if(mD3DPool == D3DPOOL_DEFAULT)
        {
            DeviceToTextureResourcesIterator it = mMapDeviceToTextureResources.find(d3d9Device);

            if (it != mMapDeviceToTextureResources.end())
            {
                StringStream ss;

                ss << "D3D9 device: 0x[" << d3d9Device << "] lost. Releasing D3D9 texture: " << mName;
                LogManager::getSingleton().logMessage(ss.str());

                TextureResources* textureResource = it->second;             

                // Just free any internal resources, don't call unload() here
                // because we want the un-touched resource to keep its unloaded status
                // after device reset.
                freeTextureResources(d3d9Device, textureResource);
                
                LogManager::getSingleton().logMessage("Released D3D9 texture: " + mName);   
            }                   
        }       
    }

    //---------------------------------------------------------------------
    void D3D9Texture::notifyOnDeviceReset(IDirect3DDevice9* d3d9Device) 
    {       
        D3D9_DEVICE_ACCESS_CRITICAL_SECTION

        if(mD3DPool == D3DPOOL_DEFAULT)
        {           
            createTextureResources(d3d9Device);
        }
    }

    //---------------------------------------------------------------------
    IDirect3DBaseTexture9* D3D9Texture::getTexture()
    {
        TextureResources* textureResources;         
        IDirect3DDevice9* d3d9Device = D3D9RenderSystem::getActiveD3D9Device();
            
        textureResources = getTextureResources(d3d9Device);     
        if (textureResources == NULL || textureResources->pBaseTex == NULL)
        {           
            createTextureResources(d3d9Device);
            textureResources = getTextureResources(d3d9Device);         
        }
        assert(textureResources); 
        assert(textureResources->pBaseTex); 

        return textureResources->pBaseTex;
    }

    //---------------------------------------------------------------------
    IDirect3DTexture9* D3D9Texture::getNormTexture()
    {
        TextureResources* textureResources;
        IDirect3DDevice9* d3d9Device = D3D9RenderSystem::getActiveD3D9Device();
        
        textureResources = getTextureResources(d3d9Device);     
        if (textureResources == NULL || textureResources->pNormTex == NULL)
        {
            createTextureResources(d3d9Device);
            textureResources = getTextureResources(d3d9Device);         
        }
        assert(textureResources); 
        assert(textureResources->pNormTex); 

        return textureResources->pNormTex;
    }

    //---------------------------------------------------------------------
    IDirect3DCubeTexture9* D3D9Texture::getCubeTexture()
    {
        TextureResources* textureResources;
        IDirect3DDevice9* d3d9Device = D3D9RenderSystem::getActiveD3D9Device();
        
        textureResources = getTextureResources(d3d9Device);     
        if (textureResources == NULL || textureResources->pCubeTex)
        {
            createTextureResources(d3d9Device);
            textureResources = getTextureResources(d3d9Device);         
        }
        assert(textureResources); 
        assert(textureResources->pCubeTex); 

        return textureResources->pCubeTex;
    }   

    /****************************************************************************************/
    D3D9RenderTexture::D3D9RenderTexture(const String &name, 
        D3D9HardwarePixelBuffer *buffer, 
        bool writeGamma, 
        uint fsaa) : RenderTexture(buffer, 0)
    {
        mName = name;
        mHwGamma = writeGamma;
        mFSAA = fsaa;       
    }
    //---------------------------------------------------------------------
    void D3D9RenderTexture::update(bool swap)
    {
        D3D9DeviceManager* deviceManager = D3D9RenderSystem::getDeviceManager();        
        D3D9Device* currRenderWindowDevice = deviceManager->getActiveRenderTargetDevice();

        if (currRenderWindowDevice != NULL)
        {
            if (currRenderWindowDevice->isDeviceLost() == false)
                RenderTexture::update(swap);
        }
        else
        {
            for (UINT i=0; i < deviceManager->getDeviceCount(); ++i)
            {
                D3D9Device* device = deviceManager->getDevice(i);

                if (device->isDeviceLost() == false)
                {
                    deviceManager->setActiveRenderTargetDevice(device);
                    RenderTexture::update(swap);
                    deviceManager->setActiveRenderTargetDevice(NULL);
                }                               
            }
        }
    }
    //---------------------------------------------------------------------
    void D3D9RenderTexture::getCustomAttribute( const String& name, void *pData )
    {
        if(name == "DDBACKBUFFER")
        {
            auto device = D3D9RenderSystem::getActiveD3D9Device();
            auto d3dBuffer = static_cast<D3D9HardwarePixelBuffer*>(mBuffer);

            if(PixelUtil::isDepth(mBuffer->getFormat()))
            {
                *static_cast<IDirect3DSurface9**>(pData) = d3dBuffer->getNullSurface(device);
                return;
            }

            if (mFSAA > 0)
            {
                // rendering to AA surface
                *(IDirect3DSurface9**)pData = d3dBuffer->getFSAASurface(device);
            }
            else
            {
                *(IDirect3DSurface9**)pData = d3dBuffer->getSurface(device);
            }
        }
        else if(name == "HWND")
        {
            *(HWND*)pData = NULL;
        }
        else if(name == "BUFFER")
        {
            *static_cast<HardwarePixelBuffer**>(pData) = mBuffer;
        }
    }    
    //---------------------------------------------------------------------
    void D3D9RenderTexture::swapBuffers()
    {
        // Only needed if we have to blit from AA surface
        if (mFSAA > 0)
        {
            D3D9DeviceManager* deviceManager = D3D9RenderSystem::getDeviceManager();                        
            D3D9HardwarePixelBuffer* buf = static_cast<D3D9HardwarePixelBuffer*>(mBuffer);

            for (UINT i=0; i < deviceManager->getDeviceCount(); ++i)
            {
                D3D9Device* device = deviceManager->getDevice(i);
                                
                if (device->isDeviceLost() == false)
                {
                    IDirect3DDevice9* d3d9Device = device->getD3D9Device();

                    HRESULT hr = d3d9Device->StretchRect(buf->getFSAASurface(d3d9Device), 0, 
                        buf->getSurface(d3d9Device), 0, D3DTEXF_NONE);

                    if (FAILED(hr))
                    {
                        OGRE_EXCEPT(Exception::ERR_INTERNAL_ERROR, 
                            "Unable to copy AA buffer to final buffer: " + String(DXGetErrorDescription(hr)), 
                            "D3D9RenderTexture::swapBuffers");
                    }
                }                               
            }                                                                       
        }           
    }   
}
