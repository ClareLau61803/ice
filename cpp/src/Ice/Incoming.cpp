// **********************************************************************
//
// Copyright (c) 2002
// ZeroC, Inc.
// Billerica, MA, USA
//
// All Rights Reserved.
//
// Ice is free software; you can redistribute it and/or modify it under
// the terms of the GNU General Public License version 2 as published by
// the Free Software Foundation.
//
// **********************************************************************

#include <Ice/Incoming.h>
#include <Ice/ObjectAdapter.h>
#include <Ice/ServantLocator.h>
#include <Ice/Object.h>
#include <Ice/Connection.h>
#include <Ice/LocalException.h>
#include <Ice/Instance.h>
#include <Ice/Properties.h>
#include <Ice/IdentityUtil.h>
#include <Ice/LoggerUtil.h>
#include <Ice/StringUtil.h>
#include <Ice/Protocol.h>

using namespace std;
using namespace Ice;
using namespace IceInternal;

IceInternal::Incoming::Incoming(const InstancePtr& instance, const ObjectAdapterPtr& adapter, Connection* connection,
				bool response, bool compress) :
    _connection(connection),
    _response(response),
    _compress(compress),
    _is(instance),
    _os(instance)
{
    _current.adapter = adapter;
}

void
IceInternal::Incoming::invoke()
{
    //
    // Read the current.
    //
    _current.id.__read(&_is);
    _is.read(_current.facet);
    _is.read(_current.operation);
    Byte b;
    _is.read(b);
    _current.mode = static_cast<OperationMode>(b);
    Int sz;
    _is.readSize(sz);
    while(sz--)
    {
	pair<string, string> pr;
	_is.read(pr.first);
	_is.read(pr.second);
	_current.ctx.insert(_current.ctx.end(), pr);
    }

    _is.startReadEncaps();

    if(_response)
    {
	assert(_os.b.size() == headerSize + 4); // Dispatch status position.
	_os.write(static_cast<Byte>(0));
	_os.startWriteEncaps();
    }

    DispatchStatus status;

    //
    // Don't put the code above into the try block below. Exceptions
    // in the code above are considered fatal, and must propagate to
    // the caller of this operation.
    //

    try
    {
	if(_current.adapter)
	{
	    _servant = _current.adapter->identityToServant(_current.id);
	    
	    if(!_servant && !_current.id.category.empty())
	    {
		_locator = _current.adapter->findServantLocator(_current.id.category);
		if(_locator)
		{
		    _servant = _locator->locate(_current, _cookie);
		}
	    }
	    
	    if(!_servant)
	    {
		_locator = _current.adapter->findServantLocator("");
		if(_locator)
		{
		    _servant = _locator->locate(_current, _cookie);
		}
	    }
	}
	    
	if(!_servant)
	{
	    status = DispatchObjectNotExist;
	}
	else
	{
	    if(!_current.facet.empty())
	    {
		ObjectPtr facetServant = _servant->ice_findFacetPath(_current.facet, 0);
		if(!facetServant)
		{
		    status = DispatchFacetNotExist;
		}
		else
		{
		    status = facetServant->__dispatch(*this, _current);
		}
	    }
	    else
	    {
		status = _servant->__dispatch(*this, _current);
	    }

	    if(_is.b.empty()) // Asynchronous dispatch?
	    {
		//
		// If this was an asynchronous dispatch, we're done
		// here.  We do *not* call finishInvoke(), because the
		// call is not finished yet.
		//
		assert(status == DispatchOK);
		return;
	    }
	}
    }
    catch(RequestFailedException& ex)
    {
	if(ex.id.name.empty())
	{
	    ex.id = _current.id;
	}
	
	if(ex.facet.empty() && !_current.facet.empty())
	{
	    ex.facet = _current.facet;
	}
	
	if(ex.operation.empty() && !_current.operation.empty())
	{
	    ex.operation = _current.operation;
	}

	warning(ex);

	if(_response)
	{
	    _os.endWriteEncaps();
	    _os.b.resize(headerSize + 4); // Dispatch status position.
	    if(dynamic_cast<ObjectNotExistException*>(&ex))
	    {
		_os.write(static_cast<Byte>(DispatchObjectNotExist));
	    }
	    else if(dynamic_cast<FacetNotExistException*>(&ex))
	    {
		_os.write(static_cast<Byte>(DispatchFacetNotExist));
	    }
	    else if(dynamic_cast<OperationNotExistException*>(&ex))
	    {
		_os.write(static_cast<Byte>(DispatchOperationNotExist));
	    }
	    else
	    {
		assert(false);
	    }
	    ex.id.__write(&_os);
	    _os.write(ex.facet);
	    _os.write(ex.operation);
	}

	finishInvoke();
	return;
    }
    catch(const LocalException& ex)
    {
	warning(ex);

	if(_response)
	{
	    _os.endWriteEncaps();
	    _os.b.resize(headerSize + 4); // Dispatch status position.
	    _os.write(static_cast<Byte>(DispatchUnknownLocalException));
	    ostringstream str;
	    str << ex;
	    _os.write(str.str());
	}

	finishInvoke();
	return;
    }
    catch(const UserException& ex)
    {
	warning(ex);

	if(_response)
	{
	    _os.endWriteEncaps();
	    _os.b.resize(headerSize + 4); // Dispatch status position.
	    _os.write(static_cast<Byte>(DispatchUnknownUserException));
	    ostringstream str;
	    str << ex;
	    _os.write(str.str());
	}

	finishInvoke();
	return;
    }
    catch(const Exception& ex)
    {
	warning(ex);

	if(_response)
	{
	    _os.endWriteEncaps();
	    _os.b.resize(headerSize + 4); // Dispatch status position.
	    _os.write(static_cast<Byte>(DispatchUnknownException));
	    ostringstream str;
	    str << ex;
	    _os.write(str.str());
	}

	finishInvoke();
	return;
    }
    catch(const std::exception& ex)
    {
	warning(string("std::exception: ") + ex.what());

	if(_response)
	{
	    _os.endWriteEncaps();
	    _os.b.resize(headerSize + 4); // Dispatch status position.
	    _os.write(static_cast<Byte>(DispatchUnknownException));
	    ostringstream str;
	    str << "std::exception: " << ex.what();
	    _os.write(str.str());
	}

	finishInvoke();
	return;
    }
    catch(...)
    {
	warning("unknown c++ exception");

	if(_response)
	{
	    _os.endWriteEncaps();
	    _os.b.resize(headerSize + 4); // Dispatch status position.
	    _os.write(static_cast<Byte>(DispatchUnknownException));
	    string reason = "unknown c++ exception";
	    _os.write(reason);
	}

	finishInvoke();
	return;
    }

    //
    // Don't put the code below into the try block above. Exceptions
    // in the code below are considered fatal, and must propagate to
    // the caller of this operation.
    //

    if(_response)
    {
	_os.endWriteEncaps();
	
	if(status != DispatchOK && status != DispatchUserException)
	{
	    assert(status == DispatchObjectNotExist ||
		   status == DispatchFacetNotExist ||
		   status == DispatchOperationNotExist);
	    
	    _os.b.resize(headerSize + 4); // Dispatch status position.
	    _os.write(static_cast<Byte>(status));
	    
	    _current.id.__write(&_os);
	    _os.write(_current.facet);
	    _os.write(_current.operation);
	}
	else
	{
	    *(_os.b.begin() + headerSize + 4) = static_cast<Byte>(status); // Dispatch status position.
	}
    }

    finishInvoke();
}

BasicStream*
IceInternal::Incoming::is()
{
    return &_is;
}

BasicStream*
IceInternal::Incoming::os()
{
    return &_os;
}

void
IceInternal::Incoming::finishInvoke()
{
    if(_locator && _servant)
    {
	_locator->finished(_current, _servant, _cookie);
    }

    _is.endReadEncaps();

    //
    // Send a response if necessary. If we don't need to send a
    // response, we still need to tell the connection that we're
    // finished with dispatching.
    //
    if(_response)
    {
	_connection->sendResponse(&_os, _compress);
    }
    else
    {
	_connection->sendNoResponse();
    }
}

void
IceInternal::Incoming::warning(const Exception& ex) const
{
    ostringstream str;
    str << ex;
    warning(str.str());
}

void
IceInternal::Incoming::warning(const string& msg) const
{
    if(_os.instance()->properties()->getPropertyAsIntWithDefault("Ice.Warn.Dispatch", 1) > 1)
    {
	Warning out(_os.instance()->logger());
	
	out << "dispatch exception: " << msg;
	out << "\nidentity: " << _current.id;
	out << "\nfacet: ";
	vector<string>::const_iterator p = _current.facet.begin();
	while(p != _current.facet.end())
	{
	    out << encodeString(*p++, "/");
	    if(p != _current.facet.end())
	    {
		out << '/';
	    }
	}
	out << "\noperation: " << _current.operation;
    }
}
