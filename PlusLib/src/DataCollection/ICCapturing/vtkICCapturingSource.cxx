/*=Plus=header=begin======================================================
Program: Plus
Copyright (c) Laboratory for Percutaneous Surgery. All rights reserved.
See License.txt for details.
=========================================================Plus=header=end*/

#include "ICCapturingListener.h"
#include "PlusConfigure.h"
#include "vtkICCapturingSource.h"
#include "vtkImageData.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkMultiThreader.h"
#include "vtkObjectFactory.h"
#include "vtkPlusChannel.h"
#include "vtkPlusDataSource.h"
#include "vtkPlusBuffer.h"
#include "vtkStreamingDemandDrivenPipeline.h"
#include "vtksys/SystemTools.hxx"
#include <tisudshl.h>

#include <ctype.h>

#include <vector>
#include <string>


vtkCxxRevisionMacro(vtkICCapturingSource, "$Revision: 1.0$");

vtkICCapturingSource* vtkICCapturingSource::Instance = 0;
vtkICCapturingSourceCleanup vtkICCapturingSource::Cleanup;

vtkICCapturingSourceCleanup::vtkICCapturingSourceCleanup(){}

vtkICCapturingSourceCleanup::~vtkICCapturingSourceCleanup()
{
  // Destroy any remaining output window.
  vtkICCapturingSource::SetInstance(NULL);
}


//----------------------------------------------------------------------------
vtkICCapturingSource::vtkICCapturingSource()
{
  this->ICBufferSize = 50; 

  this->DeviceName = NULL; 
  this->VideoNorm = NULL; 
  this->VideoFormat = NULL;
  this->FrameSize[0] = 640;
  this->FrameSize[1] = 480;
  this->InputChannel = NULL; 

  this->ClipRectangleOrigin[0]=0;
  this->ClipRectangleOrigin[1]=0;
  this->ClipRectangleSize[0]=0;
  this->ClipRectangleSize[1]=0;

  this->FrameGrabber = NULL;
  this->FrameGrabberListener = NULL; 

  this->Modified();

  this->RequireImageOrientationInConfiguration = true;

  // No need for StartThreadForInternalUpdates, as we are notified about each new frame through a callback function
}

//----------------------------------------------------------------------------
vtkICCapturingSource::~vtkICCapturingSource()
{ 
  this->Disconnect();

  if ( this->FrameGrabber != NULL) 
  {
    delete this->FrameGrabber; 
    this->FrameGrabber = NULL;
  }

  if ( this->FrameGrabberListener != NULL) 
  {
    delete this->FrameGrabberListener; 
    this->FrameGrabberListener = NULL;
  }

  SetDeviceName(NULL);
  SetVideoNorm(NULL);
  SetVideoFormat(NULL);
}

//----------------------------------------------------------------------------
// Up the reference count so it behaves like New
vtkICCapturingSource* vtkICCapturingSource::New()
{
  vtkICCapturingSource* ret = vtkICCapturingSource::GetInstance();
  ret->Register(NULL);
  return ret;
}

//----------------------------------------------------------------------------
// Return the single instance of the vtkOutputWindow
vtkICCapturingSource* vtkICCapturingSource::GetInstance()
{
  if(!vtkICCapturingSource::Instance)
  {
    // Try the factory first
    vtkICCapturingSource::Instance = (vtkICCapturingSource*)vtkObjectFactory::CreateInstance("vtkICCapturingSource");    
    if(!vtkICCapturingSource::Instance)
    {
      vtkICCapturingSource::Instance = new vtkICCapturingSource();     
    }
    if(!vtkICCapturingSource::Instance)
    {
      int error = 0;
    }
  }
  // return the instance
  return vtkICCapturingSource::Instance;
}

//----------------------------------------------------------------------------
void vtkICCapturingSource::SetInstance(vtkICCapturingSource* instance)
{
  if (vtkICCapturingSource::Instance==instance)
  {
    return;
  }
  // preferably this will be NULL
  if (vtkICCapturingSource::Instance)
  {
    vtkICCapturingSource::Instance->Delete();;
  }
  vtkICCapturingSource::Instance = instance;
  if (!instance)
  {
    return;
  }
  // user will call ->Delete() after setting instance
  instance->Register(NULL);
}

//----------------------------------------------------------------------------
std::string vtkICCapturingSource::GetSdkVersion()
{
  std::ostringstream version; 
  version << "The Imaging Source UDSHL-" << UDSHL_LIB_VERSION_MAJOR << "." << UDSHL_LIB_VERSION_MINOR; 
  return version.str(); 
}

//----------------------------------------------------------------------------
void vtkICCapturingSource::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os,indent);

}

//----------------------------------------------------------------------------
// the callback function used when there is a new frame of data received
bool vtkICCapturingSource::vtkICCapturingSourceNewFrameCallback(unsigned char * data, unsigned long size, unsigned long frameNumber)
{    
  if(data==NULL || size==0)
  {
    LOG_ERROR("No actual frame data received from the framegrabber");
    return false;
  }

  vtkICCapturingSource::GetInstance()->AddFrameToBuffer(data, size, frameNumber);    

  return true;
}

//----------------------------------------------------------------------------
// copy the Device Independent Bitmap from the VFW framebuffer into the
// vtkVideoSource framebuffer (don't do the unpacking yet)
PlusStatus vtkICCapturingSource::AddFrameToBuffer(unsigned char * dataPtr, unsigned long size, unsigned long frameNumber)
{
  if (!this->Recording)
  {
    // drop the frame, we are not recording data now
    return PLUS_SUCCESS;
  }

  vtkPlusDataSource* aSource=NULL;
  if( this->GetFirstActiveOutputVideoSource(aSource) != PLUS_SUCCESS )
  {
    LOG_ERROR("Unable to retrieve the video source in the ICCapturing device.");
    return PLUS_FAIL;
  }

  this->FrameNumber = frameNumber; 

  const int frameSize[2] = {static_cast<DShowLib::Grabber*>(FrameGrabber)->getAcqSizeMaxX(), static_cast<DShowLib::Grabber*>(FrameGrabber)->getAcqSizeMaxY()}; 
  int frameBufferBitsPerPixel = static_cast<DShowLib::Grabber*>(FrameGrabber)->getVideoFormat().getBitsPerPixel(); 
  if (frameBufferBitsPerPixel!=8)
  {
    LOG_ERROR("vtkICCapturingSource::AddFrameToBuffer: only 8-bit acquisition is supported, current frameBufferBitsPerPixel="<<frameBufferBitsPerPixel);
    return PLUS_FAIL;
  }

  PlusStatus status=PLUS_SUCCESS;
  if( (this->ClipRectangleSize[0] > 0) && (this->ClipRectangleSize[1] > 0)
    && (this->ClipRectangleSize[0]<frameSize[0] || this->ClipRectangleSize[1]<frameSize[1]))
  {
    // Clipping
    LimitClippingToValidRegion(frameSize);
    unsigned int bufferSize=this->ClipRectangleSize[0]*this->ClipRectangleSize[1];
    this->ClippedImageBuffer.resize(bufferSize);
    // Copy the pixels from full frame buffer to clipped frame buffer line-by-line
    unsigned char* fullFramePixelPtr=dataPtr+this->ClipRectangleOrigin[1]*frameSize[0]+this->ClipRectangleOrigin[0];
    unsigned char* clippedFramePixelPtr=&(this->ClippedImageBuffer[0]);
    for (int y=0; y<this->ClipRectangleSize[1]; y++)
    {
      memcpy(clippedFramePixelPtr,fullFramePixelPtr,this->ClipRectangleSize[0]);
      clippedFramePixelPtr+=this->ClipRectangleSize[0];
      fullFramePixelPtr+=frameSize[0];
    }
    status = aSource->GetBuffer()->AddItem(&(this->ClippedImageBuffer[0]), aSource->GetPortImageOrientation(), this->ClipRectangleSize, VTK_UNSIGNED_CHAR, 1, US_IMG_BRIGHTNESS, 0, this->FrameNumber); 
  }
  else
  {
    // No clipping
    status = aSource->GetBuffer()->AddItem(dataPtr, aSource->GetPortImageOrientation(), frameSize, VTK_UNSIGNED_CHAR, 1, US_IMG_BRIGHTNESS, 0, this->FrameNumber); 
  }
  this->Modified();

  return status;
}

//----------------------------------------------------------------------------
void vtkICCapturingSource::LimitClippingToValidRegion(const int frameSize[2])
{
  if (this->ClipRectangleOrigin[0]<0 || this->ClipRectangleOrigin[1]<0
    || this->ClipRectangleOrigin[0]>=frameSize[0] || this->ClipRectangleOrigin[1]>=frameSize[1])
  {
    LOG_WARNING("ClipRectangleOrigin is invalid ("<<this->ClipRectangleOrigin[0]<<", "<<this->ClipRectangleOrigin[1]<<"). The frame size is "
      <<frameSize[0]<<"x"<<frameSize[1]<<". Using (0,0) as ClipRectangleOrigin.");
    this->ClipRectangleOrigin[0]=0;
    this->ClipRectangleOrigin[1]=0;
  }
  if (this->ClipRectangleOrigin[0]+this->ClipRectangleSize[0]>=frameSize[0])
  {
    // rectangle size is out of the framSize bounds, clip it to the available size
    this->ClipRectangleSize[0]=frameSize[0]-this->ClipRectangleOrigin[0];
    LOG_WARNING("Adjusting ClipRectangleSize x to "<<this->ClipRectangleSize[0]);
  }
  if (this->ClipRectangleOrigin[1]+this->ClipRectangleSize[1]>frameSize[1])
  {
    // rectangle size is out of the framSize bounds, clip it to the available size
    this->ClipRectangleSize[1]=frameSize[1]-this->ClipRectangleOrigin[1];
    LOG_WARNING("Adjusting ClipRectangleSize y to "<<this->ClipRectangleSize[1]);
  }    
}

  
//----------------------------------------------------------------------------
PlusStatus vtkICCapturingSource::InternalConnect()
{
  if( !DShowLib::InitLibrary() )
  {
    LOG_ERROR("The IC capturing library could not be initialized");
    return PLUS_FAIL;
  }

  // Add DShowLib::ExitLibrary to the list of functions that are called on application exit.
  // It is useful because when the application is forced to exit then the destructor may not be called.
  static bool exitFunctionAdded=false;
  if (!exitFunctionAdded)
  {
    atexit( DShowLib::ExitLibrary );
    exitFunctionAdded=true;
  }

  if ( this->FrameGrabber == NULL ) 
  {
    this->FrameGrabber = new DShowLib::Grabber; 
  }

  // Set the device name (e.g. DFG/USB2-lt)
  if ( this->GetDeviceName() == NULL || !static_cast<DShowLib::Grabber*>(FrameGrabber)->openDev(this->GetDeviceName() ) ) 
  {
    LOG_ERROR("The IC capturing library could not be initialized - invalid device name: " << this->GetDeviceName() ); 
    return PLUS_FAIL;
  }

  // Set the video norm (e.g. PAL_B or NTSC_M)
  if ( this->GetVideoNorm() == NULL || !static_cast<DShowLib::Grabber*>(FrameGrabber)->setVideoNorm( this->GetVideoNorm() ) ) 
  {
    LOG_ERROR("The IC capturing library could not be initialized - invalid video norm: " << this->GetVideoNorm() ); 
    return PLUS_FAIL;
  }

  // The Y800 color format is an 8 bit monochrome format. 
  if ( !static_cast<DShowLib::Grabber*>(FrameGrabber)->setVideoFormat( this->GetDShowLibVideoFormatString().c_str() ) )
  {
    LOG_ERROR("The IC capturing library could not be initialized - invalid video format: " << this->GetDShowLibVideoFormatString() ); 
    return PLUS_FAIL;
  }

  int bitsPerPixel=static_cast<DShowLib::Grabber*>(FrameGrabber)->getVideoFormat().getBitsPerPixel();
  if (bitsPerPixel!=8)
  {
    LOG_ERROR("The IC capturing library could not be initialized - invalid bits per pixel: " << bitsPerPixel ); 
    return PLUS_FAIL;    
  }

  vtkPlusDataSource* aSource=NULL;
  if( this->GetFirstActiveOutputVideoSource(aSource) != PLUS_SUCCESS )
  {
    LOG_ERROR("Unable to retrieve the video source in the ICCapturing device.");
    return PLUS_FAIL;
  }
  aSource->GetBuffer()->SetPixelType( VTK_UNSIGNED_CHAR );  

  int frameSize[2]={0,0};
  frameSize[0]=static_cast<DShowLib::Grabber*>(FrameGrabber)->getAcqSizeMaxX();
  frameSize[1]=static_cast<DShowLib::Grabber*>(FrameGrabber)->getAcqSizeMaxY();

  if( (this->ClipRectangleSize[0] > 0) && (this->ClipRectangleSize[1] > 0) )
  {
    LimitClippingToValidRegion(frameSize);
    aSource->GetBuffer()->SetFrameSize(this->ClipRectangleSize);
  }
  else
  {
    // No clipping
    aSource->GetBuffer()->SetFrameSize(frameSize); 
  }

  if ( this->GetInputChannel() == NULL || !static_cast<DShowLib::Grabber*>(FrameGrabber)->setInputChannel( this->GetInputChannel() ) ) 
  {
    LOG_ERROR("The IC capturing library could not be initialized - invalid input channel: " << this->GetInputChannel() ); 
    return PLUS_FAIL;
  }

  if (this->FrameGrabberListener==NULL)
  {
    this->FrameGrabberListener = new ICCapturingListener(); 
  }

  this->FrameGrabberListener->SetICCapturingSourceNewFrameCallback(vtkICCapturingSource::vtkICCapturingSourceNewFrameCallback); 

  // Assign the number of buffers to the cListener object.
  this->FrameGrabberListener->setBufferSize( this->GetICBufferSize() );

  // Register the FrameGrabberListener object for the frame ready 
  // TODO: check if the listener should be removed (in disconnect, when reconnecting, ...)
  static_cast<DShowLib::Grabber*>(FrameGrabber)->addListener( FrameGrabberListener, DShowLib::GrabberListener::eFRAMEREADY );

  // Create a FrameTypeInfoArray data structure describing the allowed color formats.
  DShowLib::FrameTypeInfoArray acceptedTypes = DShowLib::FrameTypeInfoArray::createRGBArray();

  // Create the frame handler sink: 8 bit monochrome format. 
  smart_ptr<DShowLib::FrameHandlerSink> pSink = DShowLib::FrameHandlerSink::create( DShowLib::eRGB8, 1);
  //smart_ptr<DShowLib::FrameHandlerSink> pSink = DShowLib::FrameHandlerSink::create( DShowLib::eY800, 1);

  // Disable snap mode.
  pSink->setSnapMode( false );

  // Apply the sink to the grabber.
  static_cast<DShowLib::Grabber*>(FrameGrabber)->setSinkType( pSink );

  return PLUS_SUCCESS; 
}

//----------------------------------------------------------------------------
PlusStatus vtkICCapturingSource::InternalDisconnect()
{
  LOG_DEBUG("Disconnect from IC capturing");

  static_cast<DShowLib::Grabber*>(FrameGrabber)->removeListener( FrameGrabberListener );
  delete this->FrameGrabberListener; 
  this->FrameGrabberListener=NULL;

  delete FrameGrabber;
  this->FrameGrabber=NULL;

  DShowLib::ExitLibrary(); 

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkICCapturingSource::InternalStartRecording()
{
  if (!static_cast<DShowLib::Grabber*>(FrameGrabber)->startLive(false))
  {
    LOG_ERROR("Framegrabber startLive failed, cannot start the recording");
    return PLUS_FAIL;
  }
  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkICCapturingSource::InternalStopRecording()
{
  if (!static_cast<DShowLib::Grabber*>(FrameGrabber)->stopLive())
  {
    LOG_ERROR("Framegrabber stopLive failed");
    return PLUS_FAIL;
  }
  return PLUS_SUCCESS;
}

//-----------------------------------------------------------------------------
PlusStatus vtkICCapturingSource::ReadConfiguration(vtkXMLDataElement* rootConfigElement)
{
  LOG_TRACE("vtkICCapturingSource::ReadConfiguration"); 

  // This is a singleton class, so some input channels, output channels, or tools might have been already
  // defined. Clean them up before creating the new ones from XML.
  for( ChannelContainerIterator it = this->OutputChannels.begin(); it != this->OutputChannels.end(); ++it)
  {
    (*it)->UnRegister(this);
  }
  this->InputChannels.clear();
  this->OutputChannels.clear();
  for( DataSourceContainerIterator it = this->Tools.begin(); it != this->Tools.end(); ++it)
  {
    it->second->UnRegister(this);
  }
  this->Tools.clear();
  for( DataSourceContainerIterator it = this->VideoSources.begin(); it != this->VideoSources.end(); ++it)
  {
    it->second->UnRegister(this);
  }
  this->VideoSources.clear();

  XML_FIND_DEVICE_ELEMENT_REQUIRED_FOR_READING(deviceConfig, rootConfigElement);
  
  XML_READ_STRING_ATTRIBUTE_OPTIONAL(DeviceName, deviceConfig);
  XML_READ_STRING_ATTRIBUTE_OPTIONAL(VideoNorm, deviceConfig);
  XML_READ_STRING_ATTRIBUTE_OPTIONAL(VideoFormat, deviceConfig);
  XML_READ_VECTOR_ATTRIBUTE_OPTIONAL(int, 2, FrameSize, deviceConfig);

  // Only for backward compatibility
  // VideoFormat used to contain both video format and frame size, so in case frame size is defined
  // then this method parses that and updates VideoFormat and FrameSize accordingly
  ParseDShowLibVideoFormatString(this->VideoFormat);

  XML_READ_STRING_ATTRIBUTE_OPTIONAL(InputChannel, deviceConfig);
  XML_READ_SCALAR_ATTRIBUTE_OPTIONAL(int, ICBufferSize, deviceConfig);

  // clipping parameters
  XML_READ_VECTOR_ATTRIBUTE_OPTIONAL(int, 2, ClipRectangleOrigin, deviceConfig);
  XML_READ_VECTOR_ATTRIBUTE_OPTIONAL(int, 2, ClipRectangleSize, deviceConfig);

  return PLUS_SUCCESS;
}

//-----------------------------------------------------------------------------
PlusStatus vtkICCapturingSource::WriteConfiguration(vtkXMLDataElement* rootConfigElement)
{
  XML_FIND_DEVICE_ELEMENT_REQUIRED_FOR_WRITING(imageAcquisitionConfig, rootConfigElement);

  imageAcquisitionConfig->SetAttribute("DeviceName", this->DeviceName);
  imageAcquisitionConfig->SetAttribute("VideoNorm", this->VideoNorm);
  imageAcquisitionConfig->SetAttribute("VideoFormat", this->VideoFormat);
  imageAcquisitionConfig->SetVectorAttribute("FrameSize", 2, this->FrameSize);
  imageAcquisitionConfig->SetAttribute("InputChannel", this->InputChannel);
  imageAcquisitionConfig->SetIntAttribute("ICBufferSize", this->ICBufferSize);

  // clipping parameters
  imageAcquisitionConfig->SetVectorAttribute("ClipRectangleOrigin", 2, this->GetClipRectangleOrigin());
  imageAcquisitionConfig->SetVectorAttribute("ClipRectangleSize", 2, this->GetClipRectangleSize());

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkICCapturingSource::NotifyConfigured()
{
  if( this->OutputChannels.size() > 1 )
  {
    LOG_WARNING("ICCapturingSource is expecting one output channel and there are " << this->OutputChannels.size() << " channels. First output channel will be used.");
    return PLUS_FAIL;
  }

  if( this->OutputChannels.empty() )
  {
    LOG_ERROR("No output channels defined for vtkICCapturingSource. Cannot proceed." );
    this->SetCorrectlyConfigured(false);
    return PLUS_FAIL;
  }

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
void vtkICCapturingSource::ParseDShowLibVideoFormatString(const char* videoFormatFrameSizeString)
{
  if (videoFormatFrameSizeString==NULL)
  {
    // parsing failed
    return;
  }

  // videoFormatFrameSizeString sample: "Y800 (640x480)"
  std::vector<std::string> splitVideoFormatFrameSize;
  PlusCommon::SplitStringIntoTokens(videoFormatFrameSizeString, ' ', splitVideoFormatFrameSize);
  if (splitVideoFormatFrameSize.size()!=2)
  {
    // parsing failed
    return;
  }

  // splitVideoFormatFrameSize[0] is Y800
  // splitVideoFormatFrameSize[1] is (640x480), so it has to be split and parsed further

  // parse frame size
  std::vector<std::string> splitFrameSize;
  PlusCommon::SplitStringIntoTokens(splitVideoFormatFrameSize[1], 'x', splitFrameSize);
  if (splitFrameSize.size()!=2 || splitFrameSize[0].empty() || splitFrameSize[1].empty()) // (640 480)
  {
    // parsing failed
    return;
  }
  if (splitFrameSize[0][0]!='(' || splitFrameSize[1][splitFrameSize[1].size()-1]!=')')
  {
    // parsing failed
    return;
  }
  // Remove parentheses
  splitFrameSize[0].erase(splitFrameSize[0].begin());
  splitFrameSize[1].erase(splitFrameSize[1].end()-1);
  // Convert to integer
  int frameSizeX=0;
  int frameSizeY=0;
  if (PlusCommon::StringToInt(splitFrameSize[0].c_str(), frameSizeX)==PLUS_FAIL)
  {
    return;
  }
  if (PlusCommon::StringToInt(splitFrameSize[1].c_str(), frameSizeY)==PLUS_FAIL)
  {
    return;
  }

  // Parsing successful, save results
  this->SetVideoFormat(splitVideoFormatFrameSize[0].c_str());
  this->SetFrameSize(frameSizeX, frameSizeY);
}


//----------------------------------------------------------------------------
std::string vtkICCapturingSource::GetDShowLibVideoFormatString()
{
  std::ostringstream ss;
  ss << (this->GetVideoFormat()?this->GetVideoFormat():"Y800");
  ss << " (" << this->FrameSize[0] << "x" << this->FrameSize[1] << ")" << std::ends;
  std::string formatString = ss.str();
  return formatString;
}
