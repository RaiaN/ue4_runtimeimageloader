// Copyright Peter Leontev

#include "RuntimeImageUtils.h"

#include "Misc/FileHelper.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"
#include "PixelFormat.h"
#include "HAL/FileManager.h"
#include "HAL/UnrealMemory.h"
#include "Serialization/BulkData.h"
#include "Serialization/Archive.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "IImageWrapper.h"
#include "RHI.h"
#include "RenderUtils.h"

#include "Helpers/TGAHelpers.h"
#include "Helpers/PNGHelpers.h"


namespace FRuntimeImageUtils
{
    bool IsImportResolutionValid(int32 Width, int32 Height, bool bAllowNonPowerOfTwo)
    {
        // TODO: 
        const int32 MAX_TEXTURE_SIZE = 8192;

        // Calculate the maximum supported resolution utilizing the global max texture mip count
        // (Note, have to subtract 1 because 1x1 is a valid mip-size; this means a GMaxTextureMipCount of 4 means a max resolution of 8x8, not 2^4 = 16x16)
        const int32 MaximumSupportedResolution = 1 << (GMaxTextureMipCount - 1);

        bool bValid = true;

        // Check if the texture is above the supported resolution and prompt the user if they wish to continue if it is
        if (Width > MaximumSupportedResolution || Height > MaximumSupportedResolution)
        {
            bValid = false;
        }

        const bool bIsPowerOfTwo = FMath::IsPowerOfTwo(Width) && FMath::IsPowerOfTwo(Height);
        // Check if the texture dimensions are powers of two
        if (!bAllowNonPowerOfTwo && !bIsPowerOfTwo)
        {
            bValid = false;
        }

        if (Width > MAX_TEXTURE_SIZE || Height > MAX_TEXTURE_SIZE)
        {
            bValid = false;
        }

        return bValid;
    }

    bool ImportBufferAsImage(const uint8* Buffer, int32 Length, FRuntimeImageData& OutImage, FString& OutError)
    {
        QUICK_SCOPE_CYCLE_COUNTER(STAT_EvoImageUtils_ImportFileAsTexture_ImportBufferAsImage);
        
        IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));

        //
        // PNG
        //
        TSharedPtr<IImageWrapper> PngImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
        if (PngImageWrapper.IsValid() && PngImageWrapper->SetCompressed(Buffer, Length))
        {
            if (!IsImportResolutionValid(PngImageWrapper->GetWidth(), PngImageWrapper->GetHeight(), true))
            {
                OutError = FString::Printf(TEXT("Texture resolution is not supported: %d x %d"), PngImageWrapper->GetWidth(), PngImageWrapper->GetHeight());
                return false;
            }

            // Select the texture's source format
            ETextureSourceFormat TextureFormat = TSF_Invalid;
            int32 BitDepth = PngImageWrapper->GetBitDepth();
            ERGBFormat Format = PngImageWrapper->GetFormat();

            if (Format == ERGBFormat::Gray)
            {
                if (BitDepth <= 8)
                {
                    TextureFormat = TSF_G8;
                    Format = ERGBFormat::Gray;
                    BitDepth = 8;
                }
                else if (BitDepth == 16)
                {
                    // TODO: TSF_G16?
                    TextureFormat = TSF_RGBA16;
                    Format = ERGBFormat::RGBA;
                    BitDepth = 16;
                }
            }
            else if (Format == ERGBFormat::RGBA || Format == ERGBFormat::BGRA)
            {
                if (BitDepth <= 8)
                {
                    TextureFormat = TSF_BGRA8;
                    Format = ERGBFormat::BGRA;
                    BitDepth = 8;
                }
                else if (BitDepth == 16)
                {
                    TextureFormat = TSF_RGBA16;
                    Format = ERGBFormat::RGBA;
                    BitDepth = 16;
                }
            }

            if (BitDepth == 16)
            {
                OutError = TEXT("16bit PNG file is not supported");
                return false;
            }

            if (TextureFormat == TSF_Invalid)
            {
                OutError = TEXT("PNG file contains data in an unsupported format.");
                return false;
            }

            TArray<uint8> RawPNG;
            if (PngImageWrapper->GetRaw(Format, BitDepth, RawPNG))
            {
                OutImage.Init2D(
                    PngImageWrapper->GetWidth(),
                    PngImageWrapper->GetHeight(),
                    TextureFormat,
                    RawPNG.GetData()
                );
                OutImage.SRGB = BitDepth < 16;

                FPNGHelpers::FillZeroAlphaPNGData(OutImage.SizeX, OutImage.SizeY, OutImage.Format, OutImage.RawData.GetData());
            }
            else
            {
                OutError = TEXT("Failed to decode PNG.");
                return false;
            }

            return true;
        }

        //
        // JPEG
        //
        TSharedPtr<IImageWrapper> JpegImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::JPEG);
        if (JpegImageWrapper.IsValid() && JpegImageWrapper->SetCompressed(Buffer, Length))
        {
            if (!IsImportResolutionValid(JpegImageWrapper->GetWidth(), JpegImageWrapper->GetHeight(), true))
            {
                OutError = FString::Printf(TEXT("Texture resolution is not supported: %d x %d"), JpegImageWrapper->GetWidth(), JpegImageWrapper->GetHeight());
                return false;
            }

            // Select the texture's source format
            ETextureSourceFormat TextureFormat = TSF_Invalid;
            int32 BitDepth = JpegImageWrapper->GetBitDepth();
            ERGBFormat Format = JpegImageWrapper->GetFormat();

            if (Format == ERGBFormat::Gray)
            {
                if (BitDepth <= 8)
                {
                    TextureFormat = TSF_G8;
                    Format = ERGBFormat::Gray;
                    BitDepth = 8;
                }
            }
            else if (Format == ERGBFormat::RGBA)
            {
                if (BitDepth <= 8)
                {
                    TextureFormat = TSF_BGRA8;
                    Format = ERGBFormat::BGRA;
                    BitDepth = 8;
                }
            }

            if (TextureFormat == TSF_Invalid)
            {
                OutError = TEXT("JPEG file contains data in an unsupported format.");
                return false;
            }

            TArray<uint8> RawJPEG;
            if (JpegImageWrapper->GetRaw(Format, BitDepth, RawJPEG))
            {
                OutImage.Init2D(
                    JpegImageWrapper->GetWidth(),
                    JpegImageWrapper->GetHeight(),
                    TextureFormat,
                    RawJPEG.GetData()
                );
                OutImage.SRGB = BitDepth < 16;
            }
            else
            {
                OutError = TEXT("Failed to decode JPEG.");
                return false;
            }

            return true;
        }

        //
        // BMP
        //
        TSharedPtr<IImageWrapper> BmpImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::BMP);
        if (BmpImageWrapper.IsValid() && BmpImageWrapper->SetCompressed(Buffer, Length))
        {
            // Check the resolution of the imported texture to ensure validity
            if (!IsImportResolutionValid(BmpImageWrapper->GetWidth(), BmpImageWrapper->GetHeight(), true))
            {
                OutError = FString::Printf(TEXT("Texture resolution is not supported: %d x %d"), BmpImageWrapper->GetWidth(), BmpImageWrapper->GetHeight());
                return false;
            }

            TArray<uint8> RawBMP;
            if (BmpImageWrapper->GetRaw(BmpImageWrapper->GetFormat(), BmpImageWrapper->GetBitDepth(), RawBMP))
            {
                // Set texture properties.
                OutImage.Init2D(
                    BmpImageWrapper->GetWidth(),
                    BmpImageWrapper->GetHeight(),
                    TSF_BGRA8,
                    RawBMP.GetData()
                );
                
            }
            else
            {
                OutError = TEXT("Failed to decode BMP.");
                return false;
            }

            return true;
        }
       
        //
        // TGA
        //
        // Support for alpha stored as pseudo-color 8-bit TGA
        const FTGAHelpers::FTGAFileHeader* TGA = (FTGAHelpers::FTGAFileHeader*)Buffer;
        if (Length >= sizeof(FTGAHelpers::FTGAFileHeader) &&
            ((TGA->ColorMapType == 0 && TGA->ImageTypeCode == 2) ||
            // ImageTypeCode 3 is greyscale
            (TGA->ColorMapType == 0 && TGA->ImageTypeCode == 3) ||
            (TGA->ColorMapType == 0 && TGA->ImageTypeCode == 10) ||
            (TGA->ColorMapType == 1 && TGA->ImageTypeCode == 1 && TGA->BitsPerPixel == 8))
            )
        {
            // Check the resolution of the imported texture to ensure validity
            if (!IsImportResolutionValid(TGA->Width, TGA->Height, true))
            {
                OutError = FString::Printf(TEXT("Texture resolution is not supported: %d x %d"), TGA->Width, TGA->Height);
                return false;
            }

            const bool bResult = FTGAHelpers::DecompressTGA(TGA, OutImage, OutError);
            if (bResult)
            {
                if (OutImage.CompressionSettings == TC_Grayscale && TGA->ImageTypeCode == 3)
                {
                    // default grayscales to linear as they wont get compression otherwise and are commonly used as masks
                    OutImage.SRGB = false;
                }
            }
            else
            {
                OutError = TEXT("Failed to decompress TGA.");
                return false;
            }
        }
        else
        {
            OutError = TEXT("TGA file contains data in an unsupported format.");
            return false;
        }

        return true;
    }

    void ImportFileAsImage(const FString& ImageFilename, FRuntimeImageData& OutImage, FString& OutError)
    {
        QUICK_SCOPE_CYCLE_COUNTER(STAT_RuntimeImageUtils_ImportFileAsTexture);

        // TODO: 
        const int64 MAX_FILESIZE_BYTES = 999999999;

        IFileManager& FileManager = IFileManager::Get();

        if (!FileManager.FileExists(*ImageFilename))
        {
            OutError = FString::Printf(TEXT("Image does not exist: %s"), *ImageFilename);
            return;
        }

        const int64 ImageFileSizeBytes = FileManager.FileSize(*ImageFilename);
        check(ImageFileSizeBytes != INDEX_NONE);

        // check filesize
        if (ImageFileSizeBytes > MAX_FILESIZE_BYTES)
        {
            OutError = FString::Printf(TEXT("Image filesize > %d MBs): %s"), MAX_FILESIZE_BYTES, *ImageFilename);
            return;
        }
        
        TArray<uint8> ImageBuffer;
        {
            QUICK_SCOPE_CYCLE_COUNTER(STAT_RuntimeImageUtils_ImportFileAsTexture_LoadFileToArray);
            if (!FFileHelper::LoadFileToArray(ImageBuffer, *ImageFilename))
            {
                OutError = FString::Printf(TEXT("Image I/O error: %s"), *ImageFilename);
                return;
            }
            ImageBuffer.Add(0);
        }

        const FFileStatData ImageFileStatData = FileManager.GetStatData(*ImageFilename);
        OutImage.ModificationTime = FMath::Max(ImageFileStatData.CreationTime, ImageFileStatData.ModificationTime);


        if (!ImportBufferAsImage(ImageBuffer.GetData(), ImageBuffer.Num() - 1, OutImage, OutError))
        {
            return;
        }
    }

    UTexture2D* CreateDummyTexture(const FString& ImageFilename, ETextureSourceFormat ImageFormat)
    {
        check(IsInGameThread());

        const FString& BaseFilename = FPaths::GetBaseFilename(ImageFilename);

        UTexture2D* NewTexture = NewObject<UTexture2D>(
            GetTransientPackage(), 
            MakeUniqueObjectName(GetTransientPackage(), UTexture2D::StaticClass(), *BaseFilename),
            RF_Transient
        );
        NewTexture->NeverStream = true;

        {
            QUICK_SCOPE_CYCLE_COUNTER(STAT_RuntimeImageReader_ImportFileAsTexture_NewTexture);

            check(IsValid(NewTexture));

            EPixelFormat PixelFormat;
            switch (ImageFormat)
            {
                case TSF_G8:            PixelFormat = PF_G8; break;
                case TSF_G16:           PixelFormat = PF_G16; break;
                case TSF_BGRA8:         PixelFormat = PF_B8G8R8A8; break;
                case TSF_BGRE8:         PixelFormat = PF_B8G8R8A8; break;
                case TSF_RGBA16:        PixelFormat = PF_R16G16B16A16_SINT; break;
                case TSF_RGBA16F:       PixelFormat = PF_FloatRGBA; break;
                default:                PixelFormat = PF_B8G8R8A8; break;
            }

            NewTexture->PlatformData = new FTexturePlatformData();
            NewTexture->PlatformData->SizeX = 1;
            NewTexture->PlatformData->SizeY = 1;
            NewTexture->PlatformData->PixelFormat = PixelFormat;

            FTexture2DMipMap* Mip = new FTexture2DMipMap();
            NewTexture->PlatformData->Mips.Add(Mip);
            Mip->SizeX = 1;
            Mip->SizeY = 1;

            const uint32 MipBytes = Mip->SizeX * Mip->SizeY * GPixelFormats[PixelFormat].BlockBytes;
            {
                Mip->BulkData.Lock(LOCK_READ_WRITE);

                void* TextureData = Mip->BulkData.Realloc(MipBytes);

                static TArray<uint8> DummyBytes;
                DummyBytes.SetNum(MipBytes);

                FMemory::Memcpy(TextureData, DummyBytes.GetData(), MipBytes);

                Mip->BulkData.Unlock();
            }

            NewTexture->UpdateResource();
        }

        return NewTexture;
    }
}