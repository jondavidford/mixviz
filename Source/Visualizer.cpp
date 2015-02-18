/*
  ==============================================================================

    Visualizer.cpp
    Created: 28 Apr 2014 10:14:16am
    Author:  jon

  ==============================================================================
*/

#include "../JuceLibraryCode/JuceHeader.h"
#include "Visualizer.h"
#include <fftw3.h>
#include <math.h>
#include <cstdio>
#include <string>
#include <sstream>
#include <fstream>
#include <loudness/Models/DynamicPartialLoudnessGM.h>

using namespace std;

//==============================================================================
Visualizer::Visualizer()
{
    // In your constructor, you should add any child components, and
    // initialise any special settings that your component needs.
    setOpaque(true);
    startTimer(1000/30);

    // add the tracks panel
    addAndMakeVisible (loadTracksButton = new TextButton("load"));
    loadTracksButton->setButtonText (TRANS("Load track names"));
    loadTracksButton->addListener (this);
    loadTracksButton->setBounds(0, 600, 100, 20);

    addAndMakeVisible (trackSelector = new TrackSelector(this));
    trackSelector->setTopLeftPosition(0,620);

    // give settings default values
    // initialize track group arrays
    changeSettings(5000.0f, 10.0f, 0.70, 2, 0);

    audioInputBank = new loudness::TrackBank();
    audioInputBank->initialize(nTrackGroups * 2, 1, 1024, 44100);

    model = new loudness::DynamicPartialLoudnessGM("44100_IIR_23_freemid.npy");
    model->initialize(*audioInputBank);

    powerSpectrumOutput = model->getModuleOutput(2);
    roexBankOutput = model->getModuleOutput(4);
    partialLoudnessOutput = model->getModuleOutput(5);
    integratedLoudnessOutput = model->getModuleOutput(6);

    nFreqBins = roexBankOutput->getNChannels();
    cutoffFreqs = roexBankOutput->getCentreFreqs();
    output.resize(nTrackGroups * 2);
    for (int track = 0; track < nTrackGroups * 2; track++)
    {
        output[track].resize(nFreqBins);
        for (int freq = 0; freq < nFreqBins; freq++)
            output[track][freq].assign(180, 0);
    }
}

Visualizer::~Visualizer()
{
}

void Visualizer::changeSettings(const float intensityScalingConstant_,
                                const float intensityCutoffConstant_,
                                const double timeDecayConstant_,
                                const double maskingThreshold_,
                                const bool detectionMode_)
{
    nTrackGroups = 4;
    nSpatialBins = 128;
    intensityScalingConstant = intensityScalingConstant_;
    intensityCutoffConstant = intensityCutoffConstant_;
    timeDecayConstant = timeDecayConstant_;
    maskingThreshold = maskingThreshold_;
    detectionMode = detectionMode_;
}

void Visualizer::updateTracksInGroup(int groupIndex, Array<int> tracksInGroup)
{
    trackGroups.set(groupIndex, tracksInGroup);
}

void Visualizer::audioDeviceAboutToStart (AudioIODevice* device)
{
    // get info about the audio device we are connected to
    activeInputChannels = device->getActiveInputChannels();
    bufferSize = device->getCurrentBufferSizeSamples();
    numActiveChannels = activeInputChannels.countNumberOfSetBits();
    std::cout << bufferSize << std::endl;
    fs = device->getCurrentSampleRate();
}

void Visualizer::audioDeviceStopped()
{
}

// this function happens each time audio data is recieved
void Visualizer::audioDeviceIOCallback (const float** inputChannelData, int numInputChannels,
                                        float** outputChannelData, int numOutputChannels,
                                        int numSamples)
{
    // for each track group
    // NOTE: no gaps in the trackGroups array due to construction
    for (int groupIndex = 0; groupIndex < nTrackGroups; ++groupIndex)
    {
        // set inputs to model to zero
        for (int i = 0; i < numSamples; ++i)
        {
            audioInputBank->setSample(2*groupIndex, 0, i, 0); // right
            audioInputBank->setSample(2*groupIndex+1, 0, i, 0); // left
        }

        // loop over tracks in the group and add their data to the audioInputBank

        for (int targetIndex = 0; targetIndex < trackGroups[groupIndex].size(); ++targetIndex)
        {
            int targetIOIndex = trackGroups[groupIndex][targetIndex];
            // copy input L and R channel data into our input sample buffer
            for (int i = 0; i < numSamples; ++i)
            {
                audioInputBank->sumSample(2*groupIndex, 0, i, (double) inputChannelData[targetIOIndex*2][i]); // right
                audioInputBank->sumSample(2*groupIndex+1, 0, i, (double) inputChannelData[targetIOIndex*2+1][i]); // left
            }
        }
    }

    // run the model
    model->process(*audioInputBank);

    // print to cout
    double il = 0;
    for (int freq = 0; freq < nFreqBins; ++freq)
        il += partialLoudnessOutput->getSample(1, freq, 1);

    //std::cout << il << std::endl;

    // apply time decay to current output
    for (int track = 0; track < nTrackGroups * 2; ++track)
        for (int freq = 0; freq < nFreqBins; ++freq)
            for (int pos = 0; pos < 180; ++pos)
                output[track][freq][pos] *= timeDecayConstant;

    
    // add current loudness values into output matrix
    for (int track = 0; track < nTrackGroups; ++track)
    {
        for (int freq = 0; freq < nFreqBins; ++freq)
        {
            const float intensity = (float) roexBankOutput->getSample(track, freq, 0);
            if (intensity > intensityCutoffConstant)
            {
                const int spatialBin = (int) powerSpectrumOutput->getSpatialPosition(track, freq);

                // detect masking
                // loudness - partial loudness
                if (log(integratedLoudnessOutput->getSample(track, freq, 1)) - log(integratedLoudnessOutput->getSample(track, freq, 4)) > maskingThreshold)
                {
                    output[track+nTrackGroups][freq][spatialBin] += intensity;
                }
                else
                {
                    output[track][freq][spatialBin] += intensity;
                }
            }
        }
    }

    // We need to clear the output buffers before returning, in case they're full of junk..
    for (int j = 0; j < numOutputChannels; ++j)
        if (outputChannelData[j] != nullptr)
            zeromem (outputChannelData[j], sizeof (float) * (size_t) numSamples);
}

// this function is called at each timer callback,
void Visualizer::paint (Graphics& g)
{
    g.fillAll (Colours::grey);   // clear the background
    const float leftBorder = 100.0f;
    const float rightBorder = 30.0f;
    const float bottomBorder = 40.0f;
    const float topBorder = 5.0f;
    const float height = (float) getHeight();
    const float width = (float) getWidth();
    const float winHeight = height - bottomBorder - topBorder;
    const float winWidth = width - leftBorder - rightBorder;
    const float maxXIndex = 180.0f; // number of degrees
    const float maxYIndex = (float) nFreqBins;
    const float binHeight = winHeight / maxYIndex;
    const float binWidth = winWidth / maxXIndex;
    const float textWidth = 50.0f;
    const float textOffset = textWidth / 2.0f;
    const float tickHeight = 5.0f;

    g.setColour(Colours::black);
    g.fillRect(Rectangle<float>(leftBorder, topBorder, winWidth, winHeight));
    if (detectionMode)
    {
        // only draw maskers
        for (int track = 0; track < nTrackGroups; ++track)
        {
            const int idx = track + nTrackGroups;
            for (int freq = 0; freq < nFreqBins; ++freq)
            {
                for (int pos = 0; pos < 180; ++pos)
                {
                    const float intensity = output[idx][freq][pos];
                    if (intensity > intensityCutoffConstant)
                    {
                        const float xf = (float) pos; // add 90 so all values are positive
                        const float yf = (float) freq;
                        g.setColour(trackIntensityToColour(intensity,track));
                        g.fillRect(Rectangle<float>((xf + 1.0f) / maxXIndex * winWidth - binWidth + leftBorder,
                                                ((maxYIndex - yf) / maxYIndex) * winHeight - binHeight + topBorder,
                                                binWidth,
                                                binHeight));
                    }
                }
            }
        }
    }
    else
    {
        // draw the current output matrix
        for (int track = 0; track < nTrackGroups; ++track)
        {
            for (int freq = 0; freq < nFreqBins; ++freq)
            {
                for (int pos = 0; pos < 180; ++pos)
                {
                    const float intensity = output[track][freq][pos];
                    if (intensity > intensityCutoffConstant)
                    {
                        const float xf = (float) pos; // add 90 so all values are positive
                        const float yf = (float) freq;
                        g.setColour(trackIntensityToColour(intensity,track));
                        g.fillRect(Rectangle<float>((xf + 1.0f) / maxXIndex * winWidth - binWidth + leftBorder,
                                                ((maxYIndex - yf) / maxYIndex) * winHeight - binHeight + topBorder,
                                                binWidth,
                                                binHeight));
                    }
                }
            }
        }
        // draw maskers
        for (int track = 0; track < nTrackGroups; ++track)
        {
            const int idx = track + nTrackGroups;
            for (int freq = 0; freq < nFreqBins; ++freq)
            {
                for (int pos = 0; pos < 180; ++pos)
                {
                    const float intensity = output[idx][freq][pos];
                    if (intensity > intensityCutoffConstant)
                    {
                        const float xf = (float) pos; // add 90 so all values are positive
                        const float yf = (float) freq;
                        g.setColour(maskerIntensityToColour(intensity,track));
                        g.fillRect(Rectangle<float>((xf + 1.0f) / maxXIndex * winWidth - binWidth + leftBorder,
                                                ((maxYIndex - yf) / maxYIndex) * winHeight - binHeight + topBorder,
                                                binWidth,
                                                binHeight));
                    }
                }
            }
        }
    }


    // draw a line down the middle and around this box
    g.setColour(Colours::white);
    g.fillRect(Rectangle<float>(leftBorder, topBorder, winWidth, 1.0f)); // top line
    g.fillRect(Rectangle<float>(leftBorder, winHeight + topBorder, winWidth, 1.0f)); // bottom line
    g.fillRect(Rectangle<float>(leftBorder, topBorder, 1.0f, winHeight)); // left line
    g.fillRect(Rectangle<float>(width - rightBorder, topBorder, 1.0f, winHeight)); // right line
    g.fillRect(Rectangle<float>(winWidth / 2.0f + leftBorder, topBorder, 1.0f, winHeight)); // middle line

    g.setColour(Colours::black);
    // draw text and tick marks for labeling the graph
    g.drawText ("Spatial Position", 
                Rectangle<float>(winWidth / 2.0f + leftBorder - 40.0f,
                                                    winHeight + bottomBorder / 3.0f + topBorder,
                                                    100.0f,
                                                    10.0f),
                Justification(1),
                true);

    // L100
    g.fillRect (Rectangle<float>(leftBorder, winHeight + topBorder, 1.0f, tickHeight));
    g.drawText ("L100",
                Rectangle<float>(leftBorder - textOffset, winHeight + 6.0f + topBorder, textWidth, 10.0f),
                Justification(4),
                true);

    // L50
    g.fillRect (Rectangle<float>(leftBorder + winWidth / 4.0f, winHeight + topBorder, 1.0f, tickHeight));
    g.drawText ("L50",
                Rectangle<float>(leftBorder + winWidth / 4.0f - textOffset, winHeight + 6.0f + topBorder, textWidth, 10.0f),
                Justification(4),
                true);
    
    // C
    g.fillRect (Rectangle<float>(leftBorder + winWidth / 2.0f, winHeight + topBorder, 1.0f, tickHeight));
    g.drawText ("C",
                Rectangle<float>(leftBorder + winWidth / 2.0f - textOffset, winHeight + 6.0f + topBorder, textWidth, 10.0f),
                Justification(4),
                true);

    // R50
    g.fillRect (Rectangle<float>(leftBorder + 3.0f * winWidth / 4.0f, winHeight + topBorder, 1.0f, tickHeight));
    g.drawText ("R50",
                Rectangle<float>(leftBorder + 3.0f * winWidth / 4.0f - textOffset, winHeight + 6.0f + topBorder, textWidth, 10.0f),
                Justification(4),
                true);

    // R100
    g.fillRect (Rectangle<float>(leftBorder + winWidth, winHeight + topBorder, 1.0f, tickHeight));
    g.drawText ("R100",
                Rectangle<float>(leftBorder + winWidth - textOffset, winHeight + 6.0f + topBorder, textWidth, 10.0f),
                Justification(4),
                true);

    
    // draw cutoff frequency lines
    // first draw the 0 
    g.drawFittedText ("Frequency (Hz)", Rectangle<int>(0,(int)(winHeight / 2.0f + topBorder), (int)(leftBorder/2.0f), (int)(winHeight/3.0f)), Justification(1), 10);
    const float tickX = leftBorder - tickHeight;
    const float txtX = tickX - textWidth;
    g.fillRect (Rectangle<float>(tickX,  winHeight + topBorder, tickHeight, 1.0f));
    g.drawText ("0", Rectangle<float>(txtX, winHeight + topBorder, tickHeight, 10.0f), Justification(4), true);
    for (int i = 0; i < nFreqBins; i += 10)
    {
        const float yf = (float) i;
        g.fillRect (Rectangle<float>(   tickX, 
                                        ((maxYIndex - yf) / maxYIndex) * winHeight - binHeight + topBorder,
                                        tickHeight,
                                        1.0f));
        g.drawText (String((int) cutoffFreqs[i]),
                    Rectangle<float>(   txtX, 
                                        ((maxYIndex - yf) / maxYIndex) * winHeight - binHeight - 7.0f + topBorder,
                                        textWidth,
                                        10.0f),
                    Justification(12),
                    true);
    }
    
}

void Visualizer::resized()
{
    // This method is where you should set the bounds of any child
    // components that your component contains..
}

void Visualizer::timerCallback()
{
    repaint();
}

Colour Visualizer::trackIntensityToColour(const float intensity, const int track)
{
    return Colour((float)track / (float)nTrackGroups, 0.8f, intensity / intensityScalingConstant, 1.0f);
}

Colour Visualizer::maskerIntensityToColour(const float intensity, const int track)
{
    return Colour((float)track / (float)nTrackGroups, 0.2f, 1.0f, 1.0f);
}

void Visualizer::buttonClicked (Button* buttonThatWasClicked)
{
    if (buttonThatWasClicked == loadTracksButton)
    {
        FileChooser myChooser ("Please select the .xml file generated by Jack...",
                               File::getSpecialLocation (File::userHomeDirectory),
                               "*.xml");
        std::cout << "Helloooooo" << std::endl;
        if (myChooser.browseForFileToOpen())
        {
            // load the chosen file and get output sockets
            File jackFile (myChooser.getResult());
            String text = jackFile.loadFileAsString();
            String textWithNames = text.fromFirstOccurrenceOf("<cables>", 0, 1).upToFirstOccurrenceOf("</cables>", 0, 1);

            // initialize empty hashmap
            // key is MixVis input socket
            // value is track name
            StringArray trackNames;

            // while there are still output sockets
            while (textWithNames.containsWholeWord("cable"))
            {
                // first thing should be a cable with name=
                String cableText = textWithNames.fromFirstOccurrenceOf("output=",0,1).upToFirstOccurrenceOf(" type=",0,1).unquoted();
                String name = cableText.upToFirstOccurrenceOf(" input=", 0, 1).unquoted();
                int inputSocket = cableText.fromFirstOccurrenceOf(" input=\"Input Socket ", 0, 1).upToFirstOccurrenceOf("\"", 0, 1).getIntValue();

                std::cout << inputSocket << ", " << name << std::endl;
                // insert track name, adjust for 0 indexing
                trackNames.insert(inputSocket-1, name);
                textWithNames = textWithNames.fromFirstOccurrenceOf("/>",0,1);
            }

            // display the tracks with these new names
            trackSelector->makeTrackBoxes(trackNames);
        }
    }
}

void Visualizer::sliderValueChanged (Slider*)
{
}