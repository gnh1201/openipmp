/** \file OMADRMMQEncCommHandler.cpp

    Message queue implementation of OMA DRM encoding communication handler.

    The Initial Developer of the Original Code is Mutable, Inc. <br>
    Portions created by Mutable, Inc. are <br>
    Copyright (C) Mutable, Inc. 2002-2006.  All Rights Reserved. <p>

	  
*/

#include "OMADRMMQEncCommHandler.h"
#include "OMADRMServer.h"
#include "ThreadSyncFactory.h"
#include "ROAPParser.h"
#include "XMLFactory.h"

//! DRMPlugin namespace. All DRM plugin code resides in it.
namespace DRMPlugin {

/*!  \brief  Class used for handling globals.
*/
class OMAServerGlobalHandler {
public:
  /*!  \brief  Constructor.

      If fails, throws OMADRMCommException.
  */
  OMAServerGlobalHandler(): aMutex(ThreadSyncFactory::GetGlobalMutex()), server(0) {
    if (aMutex == 0) {
      throw OMADRMCommException();
    }
  }

  /*!  \brief  Destructor.

      If fails, throws OMADRMCommException.
  */
  ~OMAServerGlobalHandler() {
    if (aMutex != 0) {
      if (aMutex->Lock() == false) {
        throw OMADRMCommException();
      }
      if (server != 0) delete server; server = 0;
      if (aMutex->Release() == false) {
        throw OMADRMCommException();
      }
      delete aMutex; aMutex = 0;
    }
  }

  /*!  \brief  Get OMA DRM server.

      \param  logger      message logger.

      \returns  If successful, OMA DRM server. If failed, zero pointer.
  */
  OMADRMServer* GetServer(DRMLogger& logger) {
    if (aMutex != 0) {
      if (aMutex->Lock() == true) {
        if (server == 0) {
#if defined (WIN32)
          IXMLDocument* doc = XMLFactory::DecodeDocumentFromFile("\\p12\\omaserver\\OMADRMServerInfo.xml", logger);
#elif defined (LINUX)
          IXMLDocument* doc = XMLFactory::DecodeDocumentFromFile("/home/danijelk/work/p12/omaserver/OMADRMServerInfo.xml", logger);
#else
          IXMLDocument* doc = 0;
#endif
          if (doc != 0) {
            IXMLElement* root = doc->GetRootElement();
            if (root != 0) {
              try {
                server = new OMADRMServer(root, logger);
              }
              catch (OMADRMServerException) {
                if (server != 0) delete server; server = 0;
              }
            }
            delete doc;
          }
        }
        if (aMutex->Release() == false) {
          if (server != 0) delete server; server = 0;
        }
      }
    }
    if (server == 0) logger.UpdateLog("OMAServerGlobalHandler::GetAgent: cannot create server...");
    return server;
  }

private:
  IMutex* aMutex;
  OMADRMServer* server;
};

static OMAServerGlobalHandler omaServerGlobalHandler;

/*! \brief  Parse message and pass it to agent or server to handle.

    If there is no response, out will be empty string ("").

    \param  in            input, input message.
    \param  out           output, output message.
    \param  agent         input, OMA DRM encoding agent.
    \param  server        input, OMA DRM server.
    \param  logger        input, message logger.

    \returns  Boolean indicating success or failure.
*/
static bool ParseAndHandle(const std::string& in, std::string& out, IOMADRMEncAgent* agent, OMADRMServer* server, DRMLogger& logger) {
  out = "";
  if ((agent == 0) || (server == 0)) {
    return false;
  }

  IXMLDocument* doc = XMLFactory::DecodeDocument(in, logger);
  if (doc == 0) {
    return false;
  }
  IXMLElement* xml = doc->GetRootElement();

  try {
    //  Check message type.
    if (xml->GetName() == "addContentKeyRequest") {
      SPtr<AddContentKeyRequest> request = ROAPParser::ParseAddContentKeyRequest(xml, logger);
      NZSPtr<AddContentKeyResponse> response = server->HandleAddContentKeyRequest(request);
      out = response->XmlEncode();
    } else if (xml->GetName() == "addContentKeyResponse") {
      SPtr<AddContentKeyResponse> response = ROAPParser::ParseAddContentKeyResponse(xml, logger);
      agent->HandleAddContentKeyResponse(response);
    } else if (xml->GetName() == "addDeviceRightsRequest") {
      SPtr<AddDeviceRightsRequest> request = ROAPParser::ParseAddDeviceRightsRequest(xml, logger);
      NZSPtr<AddDeviceRightsResponse> response = server->HandleAddDeviceRightsRequest(request);
      out = response->XmlEncode();
    } else if (xml->GetName() == "addDeviceRightsResponse") {
      SPtr<AddDeviceRightsResponse> response = ROAPParser::ParseAddDeviceRightsResponse(xml, logger);
      agent->HandleAddDeviceRightsResponse(response);
    } else {
      throw ROAPParserException();
    }
  }
  catch (ROAPParserException) {
    delete doc;
    return false;
  }
  catch (GCException) {
    delete doc;
    return false;
  }

  delete doc;
  return true;
}

/*! \brief  Delivering thread function.

    \param  lpParam       input, thread parameters.
*/
static void DelivererThread(void* lpParam) {
  OMADRMMQEncCommHandlerParams* params = (OMADRMMQEncCommHandlerParams*)lpParam;

  while (true) {
    if (params->delivererStop->Wait(100) == true) {
      //  Stop thread.
      if (params->delivererExit->Set() == false) {
        //  Error. What to do?
      }
      //  Exit.
      return;
//      ExitThread(0);
    }

    //  Get mutex.
    if (params->mutex->Lock(1000) == true) {
      if ((params->agent == 0) || (params->server == 0)) {
        params->mutex->Release();
        return;
//        ExitThread(-1);
      }
      //  Do the work.
      if (params->messageQ.empty() == false) {
        std::string in = params->messageQ.front();
        params->messageQ.pop();
        std::string out;
        ParseAndHandle(in, out, params->agent, params->server, params->commLogger);
        if (out != "") {
          params->messageQ.push(out);
        }
      }
      params->mutex->Release();
    }
  }
}

/*! \brief  Constructor.

    \param  logger      message logger.

    If fails, throws OMADRMCommException.
*/
OMADRMMQEncCommHandler::OMADRMMQEncCommHandler(DRMLogger& logger): deliverer(0), params(0, std::queue<std::string>(), ThreadSyncFactory::GetMutex(logger), ThreadSyncFactory::GetEvent(logger), ThreadSyncFactory::GetEvent(logger), logger, omaServerGlobalHandler.GetServer(logger)), commLogger(logger) {
  if ((params.mutex == 0) || (params.delivererStop == 0) || (params.delivererExit == 0)) {
    if (params.mutex != 0) delete params.mutex;
    if (params.delivererStop != 0) delete params.delivererStop;
    if (params.delivererExit != 0) delete params.delivererExit;
    commLogger.UpdateLog("OMADRMMQEncCommHandler::ctor: cannot create sync objects...");
    throw OMADRMCommException();
  }
  if (params.server == 0) {
    throw OMADRMCommException();
  }
}

/*! \brief  Destructor.

    If fails, throws OMADRMCommException.
*/
OMADRMMQEncCommHandler::~OMADRMMQEncCommHandler() {
  if ((params.mutex == 0) || (params.delivererStop == 0) || (params.delivererExit == 0)) {
    commLogger.UpdateLog("OMADRMMQEncCommHandler::dtor: missing sync objects...");
    throw OMADRMCommException();
  }

  if (params.mutex->Lock() == false) {
    commLogger.UpdateLog("OMADRMMQEncCommHandler::dtor: cannot lock sync mutex...");
    throw OMADRMCommException();
  }

  //  Got mutex, stop delivering thread and clean up everything.
  //  Tell deliverer to stop.
  if (params.delivererStop->Set() == false) {
    //  Error.
    commLogger.UpdateLog("OMADRMMQEncCommHandler::dtor: cannot set stop event...");
    params.mutex->Release();
    throw OMADRMCommException();
  }
  //  Wait till deliverer exits.
  if (deliverer != 0) {
    if (params.mutex->Release() == false) {
      commLogger.UpdateLog("OMADRMMQEncCommHandler::dtor: cannot release sync mutex...");
      throw OMADRMCommException();
    }
    if (params.delivererExit->Wait() == false) {
      commLogger.UpdateLog("OMADRMMQEncCommHandler::dtor: exit event wait timeout...");
      params.mutex->Release();
      throw OMADRMCommException();
    }
    delete deliverer; deliverer = 0;
    if (params.mutex->Lock() == false) {
      commLogger.UpdateLog("OMADRMMQEncCommHandler::dtor: cannot lock sync mutex...");
      throw OMADRMCommException();
    }
  }

  //  Deliverer has exited, now clean up everything.
  params.agent = 0;
  params.mutex->Release();
  delete params.mutex;
  delete params.delivererStop;
  delete params.delivererExit;
}

/*! \brief  Send add content encryption key request.

    \param  request             input, add content encryption key request.
    \param  url                 input, URL where to send the message.

    \returns  Boolean indicating success or failure.
*/
bool OMADRMMQEncCommHandler::SendAddContentKeyRequest(const NZSPtr<AddContentKeyRequest>&
    request, const NZSPtr<URIValue>& url) {
  if (params.mutex->Lock() == false) {
    commLogger.UpdateLog("OMADRMMQEncCommHandler::SendAddContentKeyRequest: cannot lock sync mutex...");
    return false;
  }
  params.messageQ.push(request->XmlEncode());
  return params.mutex->Release();
}

/*! \brief  Send add rights for device for content request.

    \param  request             input, add rights for device for content request.
    \param  url                 input, URL where to send the message.

    \returns  Boolean indicating success or failure.
*/
bool OMADRMMQEncCommHandler::SendAddDeviceRightsRequest(const NZSPtr<AddDeviceRightsRequest>&
    request, const NZSPtr<URIValue>& url) {
  if (params.mutex->Lock() == false) {
    commLogger.UpdateLog("OMADRMMQEncCommHandler::SendAddDeviceRightsRequest: cannot lock sync mutex...");
    return false;
  }
  params.messageQ.push(request->XmlEncode());
  return params.mutex->Release();
}

/*! \brief  Set up all neccessary data and start message delivering/receiving.

    \param  omaagent          input, OMA DRM encoding agent.

    \returns  Boolean indicating success or failure.
*/
bool OMADRMMQEncCommHandler::Run(IOMADRMEncAgent* omaagent) {
  if (omaagent == 0) {
    commLogger.UpdateLog("OMADRMMQEncCommHandler::Run: zero agent...");
    return false;
  }
  params.agent = omaagent;
  deliverer = ThreadSyncFactory::GetThread(DelivererThread, &params, commLogger);
  if (deliverer == 0) commLogger.UpdateLog("OMADRMMQEncCommHandler::Run: cannot create thread...");

  return (deliverer != 0);
}

} // namespace DRMPlugin
