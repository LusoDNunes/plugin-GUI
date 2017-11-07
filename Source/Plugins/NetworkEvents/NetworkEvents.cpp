/*
   ------------------------------------------------------------------

   This file is part of the Open Ephys GUI
   Copyright (C) 2016 Open Ephys

   ------------------------------------------------------------------

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <stdio.h>
#include "NetworkEvents.h"
#include "NetworkEventsEditor.h"


const int MAX_MESSAGE_LENGTH = 64000;


#ifdef WIN32
    #include <windows.h>
#else
    #include <unistd.h>
#endif


StringTS::StringTS()
{
    str = nullptr;
    len= 0;
    timestamp = 0;
}


std::vector<String> StringTS::splitString (char sep)
{
    String S ((const char*)str, len);
    String curr;

    std::list<String> ls;
    for (int k = 0; k < S.length(); ++k)
    {
        if (S[k] != sep)
        {
            curr+=S[k];
        }
        else
        {
            ls.push_back (curr);
            while (S[k] == sep && k < S.length())
                ++k;

            curr = "";
            if (S[k] != sep && k < S.length())
                curr += S[k];
        }
    }
    if (S.length() > 0)
    {
        if (S[S.length() - 1] != sep)
            ls.push_back (curr);
    }

    std::vector<String> Svec (ls.begin(), ls.end());
    return Svec;
}


StringTS::StringTS (MidiMessage& event)
{
    const uint8* dataptr = event.getRawData();
    const int bufferSize = event.getRawDataSize();
    len = bufferSize - 6 - 8; // -6 for initial event prefix, -8 for timestamp at the end

    memcpy (&timestamp, dataptr + 6 + len, 8); // remember to skip first six bytes
    str = new uint8[len];
    memcpy (str,dataptr + 6, len);
}


StringTS& StringTS::operator= (const StringTS& rhs)
{
    delete (str);
    len = rhs.len;
    str = new uint8[len];
    memcpy (str,rhs.str,len);
    timestamp = rhs.timestamp;

    return *this;
}


String StringTS::getString()
{
    return String ((const char*)str,len);
}


StringTS::StringTS (String S)
{
    Time t;
    str = new uint8[S.length()];
    memcpy (str, S.toRawUTF8(), S.length());
    timestamp = t.getHighResolutionTicks();

    len = S.length();
}


StringTS::StringTS (String S, int64 ts_software)
{
    str = new uint8[S.length()];
    memcpy (str, S.toRawUTF8(), S.length());
    timestamp = ts_software;

    len = S.length();
}


StringTS::StringTS (const StringTS& s)
{
    str = new uint8[s.len];
    memcpy (str, s.str, s.len);
    timestamp = s.timestamp;
    len = s.len;
}


StringTS::StringTS (unsigned char* buf, int _len, int64 ts_software) 
    : len       (_len)
    ,timestamp  (ts_software)
{
    str = new juce::uint8[len];
    for (int k = 0; k < len; ++k)
        str[k] = buf[k];
}


StringTS::~StringTS()
{
    delete str;
}


/*********************************************/
void* NetworkEvents::zmqcontext = nullptr;

NetworkEvents::NetworkEvents()
    : GenericProcessor  ("Network Events")
    , Thread            ("NetworkThread")
    , threshold         (200.0)
    , bufferZone        (5.0f)
    , state             (false)
{
    setProcessorType (PROCESSOR_TYPE_SOURCE);

    createZmqContext();

    firstTime = true;
    responder = nullptr;
    urlport = 5556;
    threadRunning = false;

    opensocket();

    sendSampleCount = false; // disable updating the continuous buffer sample counts,
    // since this processor only sends events
    shutdown = false;
}


void NetworkEvents::setNewListeningPort (int port)
{
    // first, close existing thread.
    closesocket();

    // allow some time for thread to quit
#ifdef WIN32
    Sleep (300);
#else
    usleep (300 * 1000);
#endif

    urlport = port;
    opensocket();
}


NetworkEvents::~NetworkEvents()
{
    shutdown = true;
    closesocket();
}


bool NetworkEvents::closesocket()
{
    std::cout << "Disabling network node" << std::endl;

#ifdef ZEROMQ
    if (threadRunning)
    {
		lock.enter();
        zmq_close (responder);
        zmq_ctx_destroy (zmqcontext); // this will cause the thread to exit
        zmqcontext = nullptr;
		lock.exit();

		if (!stopThread(500))
		{
			std::cerr << "Network thread timeout. Forcing thread termination, system could be lefr in an unstable state" << std::endl;
		}

        if (! shutdown)
            createZmqContext();// and this will take care that processor graph doesn't attempt to delete the context again
	
    }
#endif
    return true;
}


void NetworkEvents::createEventChannels()
{
	EventChannel* chan = new EventChannel(EventChannel::TEXT, 1, MAX_MESSAGE_LENGTH, CoreServices::getGlobalSampleRate(), this);
	chan->setName("Network messages");
	chan->setDescription("Messages received through the network events module");
	chan->setIdentifier("external.network.rawData");
	chan->addEventMetaData(new MetaDataDescriptor(MetaDataDescriptor::INT64, 1, "Software timestamp",
		"OS high resolution timer count when the event was received", "timestamp.software"));
	eventChannelArray.add(chan);
	messageChannel = chan;
}


AudioProcessorEditor* NetworkEvents::createEditor ()
{
    editor = new NetworkEventsEditor (this, true);

    return editor;
}


void NetworkEvents::setParameter (int parameterIndex, float newValue)
{
    /*
       editor->updateParameterButtons(parameterIndex);

       Parameter& p =  parameters.getReference(parameterIndex);
       p.setValue(newValue, 0);

       threshold = newValue;
    */
    //std::cout << float(p[0]) << std::endl;
}


void NetworkEvents::initSimulation()
{
    Time t;

    const int64 secondsToTicks = t.getHighResolutionTicksPerSecond();
    simulationStartTime = 3 * secondsToTicks + t.getHighResolutionTicks(); // start 10 seconds after

    simulation.push (StringTS ("ClearDesign",    simulationStartTime));
    simulation.push (StringTS ("NewDesign Test", simulationStartTime + 0.5 * secondsToTicks));
    simulation.push (StringTS ("AddCondition Name GoRight TrialTypes 1 2 3", simulationStartTime + 0.6 * secondsToTicks));
    simulation.push (StringTS ("AddCondition Name GoLeft TrialTypes 4 5 6",  simulationStartTime + 0.6 * secondsToTicks));
}


void NetworkEvents::simulateDesignAndTrials ()
{
    Time t;
    while (simulation.size() > 0)
    {
        int64 currenttime = t.getHighResolutionTicks();
        StringTS S = simulation.front();
        if (currenttime > S.timestamp)
        {
            // handle special messages
            handleSpecialMessages (S);

            postTimestamppedStringToMidiBuffer (S);
            //getUIComponent()->getLogWindow()->addLineToLog(S.getString());
            simulation.pop();
        }
        else
        {
            break;
        }
    }

}

void NetworkEvents::postTimestamppedStringToMidiBuffer (StringTS s)
{
	MetaDataValueArray md;
	md.add(new MetaDataValue(MetaDataDescriptor::INT64, 1, &s.timestamp));
	TextEventPtr event = TextEvent::createTextEvent(messageChannel, CoreServices::getGlobalTimestamp(), String::fromUTF8(reinterpret_cast<const char*>(s.str), s.len), md);
	addEvent(messageChannel, event, 0);
}


void NetworkEvents::simulateStopRecord()
{
    Time t;
    simulation.push (StringTS ("StopRecord", t.getHighResolutionTicks()));
}


void NetworkEvents::simulateStartRecord()
{
    Time t;
    simulation.push (StringTS ("StartRecord", t.getHighResolutionTicks()));
}


void NetworkEvents::simulateSingleTrial()
{
    std::cout << "Simulating trial." << std::endl;

    const int numTrials = 1;
    const float ITI         = 0.7;
    const float TrialLength = 0.4;

    Time t;

    if (firstTime)
    {
        firstTime = false;
        initSimulation();
    }

    int64 secondsToTicks = t.getHighResolutionTicksPerSecond();
    simulationStartTime = 3 * secondsToTicks + t.getHighResolutionTicks(); // start 10 seconds after

    // trial every 5 seconds
    for (int k = 0; k < numTrials; ++k)
    {
        simulation.push (StringTS ("TrialStart", simulationStartTime + ITI * k * secondsToTicks));

        if (k % 2 == 0)
            // 100 ms after trial start
            simulation.push (StringTS ("TrialType 2", simulationStartTime + (ITI * k + 0.1) * secondsToTicks));
        else
            // 100 ms after trial start
            simulation.push (StringTS ("TrialType 4", simulationStartTime + (ITI * k + 0.1) * secondsToTicks));

        // 100 ms after trial start
        simulation.push (StringTS ("TrialAlign",     simulationStartTime + (ITI * k + 0.1)         * secondsToTicks));
        // 300 ms after trial start
        simulation.push (StringTS ("TrialOutcome 1", simulationStartTime + (ITI * k + 0.3)         * secondsToTicks));
        // 400 ms after trial start
        simulation.push (StringTS ("TrialEnd",       simulationStartTime + (ITI * k + TrialLength) * secondsToTicks));
    }
}


String NetworkEvents::handleSpecialMessages (StringTS msg)
{
    /*
    std::vector<String> input = msg.splitString(' ');
    if (input[0] == "StartRecord")
    {
    	 getUIComponent()->getLogWindow()->addLineToLog("Remote triggered start recording");

    	if (input.size() > 1)
    	{
    		getUIComponent()->getLogWindow()->addLineToLog("Remote setting session name to "+input[1]);
    		// session name was also given.
    		getProcessorGraph()->getRecordNode()->setDirectoryName(input[1]);
    	}
        const MessageManagerLock mmLock;
    	getControlPanel()->recordButton->setToggleState(true,true);
    	return String("OK");
    //	getControlPanel()->placeMessageInQueue("StartRecord");
    } if (input[0] == "SetSessionName")
    {
    		getProcessorGraph()->getRecordNode()->setDirectoryName(input[1]);
    } else if (input[0] == "StopRecord")
    {
    	const MessageManagerLock mmLock;
    	//getControlPanel()->placeMessageInQueue("StopRecord");
    	getControlPanel()->recordButton->setToggleState(false,true);
    	return String("OK");
    } else if (input[0] == "ProcessorCommunication")
    {
    	ProcessorGraph *g = getProcessorGraph();
    	Array<GenericProcessor*> p = g->getListOfProcessors();
    	for (int k=0;k<p.size();k++)
    	{
    		if (p[k]->getName().toLowerCase() == input[1].toLowerCase())
    		{
    			String Query="";
    			for (int i=2;i<input.size();i++)
    			{
    				if (i == input.size()-1)
    					Query+=input[i];
    				else
    					Query+=input[i]+" ";
    			}

    			return p[k]->interProcessorCommunication(Query);
    		}
    	}

    	return String("OK");
    }

    */

    /** Start/stop data acquisition */
    String s = msg.getString();

    /** Command is first substring */
    StringArray inputs = StringArray::fromTokens (s, " ");
    String cmd = String (inputs[0]);

    const MessageManagerLock mmLock;
    if (cmd.compareIgnoreCase ("StartAcquisition") == 0)
    {
        if (! CoreServices::getAcquisitionStatus())
        {
            CoreServices::setAcquisitionStatus (true);
        }
        return String ("StartedAcquisition");
    }
    else if (cmd.compareIgnoreCase ("StopAcquisition") == 0)
    {
        if (CoreServices::getAcquisitionStatus())
        {
            CoreServices::setAcquisitionStatus (false);
        }
        return String ("StoppedAcquisition");
    }
    else if (String ("StartRecord").compareIgnoreCase (cmd) == 0)
    {
        if (! CoreServices::getRecordingStatus() 
            && CoreServices::getAcquisitionStatus())
        {
            /** First set optional parameters (name/value pairs)*/
            if (s.contains ("="))
            {
                String params = s.substring (cmd.length());
                StringPairArray dict = parseNetworkMessage (params);

                StringArray keys = dict.getAllKeys();
                for (int i = 0; i < keys.size(); ++i)
                {
                    String key   = keys[i];
                    String value = dict[key];

                    if (key.compareIgnoreCase ("CreateNewDir") == 0)
                    {
                        if (value.compareIgnoreCase ("1") == 0)
                        {
                            CoreServices::createNewRecordingDir();
                        }
                    }
                    else if (key.compareIgnoreCase ("RecDir") == 0)
                    {
                        CoreServices::setRecordingDirectory (value);
                    }
                    else if (key.compareIgnoreCase ("PrependText") == 0)
                    {
                        CoreServices::setPrependTextToRecordingDir (value);
                    }
                    else if (key.compareIgnoreCase ("AppendText") == 0)
                    {
                        CoreServices::setAppendTextToRecordingDir (value);
                    }
                }
            }

            /** Start recording */
            CoreServices::setRecordingStatus (true);
            return String ("StartedRecording");
        }
    }
    else if (String ("StopRecord").compareIgnoreCase (cmd) == 0)
    {
        if (CoreServices::getRecordingStatus())
        {
            CoreServices::setRecordingStatus (false);
            return String ("StoppedRecording");
        }
    }
    else if (cmd.compareIgnoreCase ("IsAcquiring") == 0)
    {
        String status = CoreServices::getAcquisitionStatus() ? String ("1") : String ("0");
        return status;
    }
    else if (cmd.compareIgnoreCase ("IsRecording") == 0)
    {
        String status = CoreServices::getRecordingStatus() ? String ("1") : String ("0");
        return status;
    }
    else if (cmd.compareIgnoreCase ("GetRecordingPath") == 0)
    {
        File file = CoreServices::RecordNode::getRecordingPath();
        String msg (file.getFullPathName());
        return msg;
    }
    else if (cmd.compareIgnoreCase ("GetRecordingNumber") == 0)
    {
        String status;
        status += (CoreServices::RecordNode::getRecordingNumber() + 1);
        return status;
    }
    else if (cmd.compareIgnoreCase ("GetExperimentNumber") == 0)
    {
        String status;
        status += CoreServices::RecordNode::getExperimentNumber();
        return status;
    }

    return String ("NotHandled");
}


void NetworkEvents::process (AudioSampleBuffer& buffer)
{
    setTimestampAndSamples(CoreServices::getGlobalTimestamp(),0);

    lock.enter();
    while (! networkMessagesQueue.empty())
    {
        StringTS msg = networkMessagesQueue.front();
        postTimestamppedStringToMidiBuffer (msg);
        CoreServices::sendStatusMessage ( ("Network event received: " + msg.getString()).toRawUTF8());
        networkMessagesQueue.pop();
    }

    lock.exit();
}


void NetworkEvents::opensocket()
{
    startThread();
}


void NetworkEvents::run()
{
#ifdef ZEROMQ
    responder = zmq_socket (zmqcontext, ZMQ_REP);
    String url= String ("tcp://*:") + String (urlport);
    int rc = zmq_bind (responder, url.toRawUTF8());

    if (rc != 0)
    {
        // failed to open socket?
        std::cout << "Failed to open socket: " << zmq_strerror (zmq_errno()) << std::endl;
        return;
    }

    threadRunning = true;
    unsigned char* buffer = new unsigned char[MAX_MESSAGE_LENGTH];
    int result = -1;

    while (threadRunning)
    {
		lock.enter();
        result = zmq_recv (responder, buffer, MAX_MESSAGE_LENGTH - 1, 0);  // blocking
		lock.exit();

        juce::int64 timestamp_software = timer.getHighResolutionTicks();

        if (result < 0) // will only happen when responder dies.
            break;

        StringTS Msg (buffer, result, timestamp_software);
        if (result > 0)
        {
            lock.enter();
            networkMessagesQueue.push (Msg);
            lock.exit();

            //std::cout << "Received message!" << std::endl;
            // handle special messages
            String response = handleSpecialMessages (Msg);

            zmq_send (responder, response.getCharPointer(), response.length(), 0);
        }
        else
        {
            String zeroMessageError = "Recieved Zero Message?!?!?";
            //std::cout << "Received Zero Message!" << std::endl;

            zmq_send (responder, zeroMessageError.getCharPointer(), zeroMessageError.length(), 0);
        }
    }

    zmq_close (responder);

    delete[] buffer;
    threadRunning = false;

    return;
#endif
}


int NetworkEvents::getDefaultNumOutputs() const
{
    return 0;
}

bool NetworkEvents::isReady()
{
    return true;
}


float NetworkEvents::getDefaultSampleRate() const
{
    return 30000.0f;
}


float NetworkEvents::getDefaultBitVolts() const
{
    return 0.05f;
}

void NetworkEvents::setEnabledState (bool newState)
{
    isEnabled = newState;
}


void NetworkEvents::saveCustomParametersToXml (XmlElement* parentElement)
{
    XmlElement* mainNode = parentElement->createNewChildElement ("NETWORKEVENTS");
    mainNode->setAttribute ("port", urlport);
}


void NetworkEvents::loadCustomParametersFromXml()
{
    if (parametersAsXml != nullptr)
    {
        forEachXmlChildElement (*parametersAsXml, mainNode)
        {
            if (mainNode->hasTagName ("NETWORKEVENTS"))
            {
                setNewListeningPort (mainNode->getIntAttribute("port"));
            }
        }
    }
}


void NetworkEvents::createZmqContext()
{
#ifdef ZEROMQ
	lock.enter();
    if (zmqcontext == nullptr)
        zmqcontext = zmq_ctx_new(); //<-- this is only available in version 3+
	lock.exit();
#endif
}


StringPairArray NetworkEvents::parseNetworkMessage (String msg)
{
    StringArray splitted;
    splitted.addTokens (msg, "=", "");

    StringPairArray dict = StringPairArray();
    String key   = "";
    String value = "";

    for (int i = 0; i < splitted.size() - 1; ++i)
    {
        String s1 = splitted[i];
        String s2 = splitted[i + 1];

        /** Get key */
        if (! key.isEmpty())
        {
            if (s1.contains (" "))
            {
                int i1 = s1.lastIndexOf (" ");
                key = s1.substring (i1 + 1);
            }
            else
            {
                key = s1;
            }
        }
        else
        {
            key = s1.trim();
        }

        /** Get value */
        if (i < splitted.size() - 2)
        {
            int i1 = s2.lastIndexOf (" ");
            value = s2.substring (0, i1);
        }
        else
        {
            value = s2;
        }

        dict.set (key, value);
    }

    return dict;
}
