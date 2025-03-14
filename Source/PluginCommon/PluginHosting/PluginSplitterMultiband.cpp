#include "PluginSplitterMultiband.h"

FFTProvider::FFTProvider() : _buffer(nullptr), _outputs(nullptr), _fft(FFT_ORDER) {
    _buffer = new float[FFT_SIZE];
    _outputs = new float[NUM_OUTPUTS];

    juce::FloatVectorOperations::fill(_buffer, 0, NUM_OUTPUTS);
    juce::FloatVectorOperations::fill(_outputs, 0, NUM_OUTPUTS);

    for (auto& env : _envs) {
        env.setAttackTimeMs(0.1);
        env.setReleaseTimeMs(2);
        env.setFilterEnabled(false);
    }
}

FFTProvider::~FFTProvider() {
    WECore::AudioSpinLock lock(_fftMutex);
    delete[] _buffer;
    delete[] _outputs;
}

void FFTProvider::setSampleRate(double sampleRate) {
    for (auto& env : _envs) {
        env.setSampleRate(sampleRate);
    }
}

void FFTProvider::reset() {
    for (auto& env : _envs) {
        env.reset();
    }
}

void FFTProvider::processBlock(juce::AudioBuffer<float>& buffer) {
    WECore::AudioSpinTryLock lock(_fftMutex);
    if (lock.isLocked()) {
        const size_t numBuffersRequired {static_cast<size_t>(
            std::ceil(static_cast<double>(buffer.getNumSamples()) / FFT_SIZE)
        )};

        for (size_t bufferNumber {0}; bufferNumber < numBuffersRequired; bufferNumber++) {
            // Calculate how many samples need to be processed in this chunk
            const size_t numSamplesRemaining {buffer.getNumSamples() - (bufferNumber * FFT_SIZE)};
            const size_t numSamplesToCopy {
                std::min(numSamplesRemaining, static_cast<size_t>(FFT_SIZE))
            };

            const float* const leftBufferInputStart {buffer.getReadPointer(0) + bufferNumber * FFT_SIZE};
            const float* const rightBufferInputStart {buffer.getReadPointer(1) + bufferNumber * FFT_SIZE};

            // The input buffer size might be smaller than the FFT buffer size, so then we need to
            // append the the existing FFT buffer by shifting it back and adding the new samples to
            // the end
            float* const fillStart {_buffer + FFT_SIZE - numSamplesToCopy};
            juce::FloatVectorOperations::copy(_buffer, fillStart, numSamplesToCopy);

            // Add the left and right buffers
            juce::FloatVectorOperations::add(fillStart, leftBufferInputStart, rightBufferInputStart, numSamplesToCopy);
            juce::FloatVectorOperations::multiply(fillStart, 0.5, numSamplesToCopy);

            // Perform the FFT
            _fft.performFrequencyOnlyForwardTransform(_buffer);

            // Run each FFT output bin through and envelope follower so that it is smoothed when
            // displayed on the UI
            for (int index {0}; index < NUM_OUTPUTS; index++) {
                _outputs[index] = _envs[index].getNextOutput(_buffer[index]);
            }
        }
    }
}

PluginSplitterMultiband::PluginSplitterMultiband(std::function<float(int, MODULATION_TYPE)> getModulationValueCallback, bool isStereo)
        : PluginSplitter(DEFAULT_NUM_CHAINS, getModulationValueCallback) {
    juce::Logger::writeToLog("Constructed PluginSplitterMultiband");

    _crossover.setIsStereo(isStereo);
}

PluginSplitterMultiband::PluginSplitterMultiband(std::vector<PluginChainWrapper>& chains, std::function<float(int, MODULATION_TYPE)> getModulationValueCallback, bool isStereo)
        : PluginSplitter(chains, DEFAULT_NUM_CHAINS, getModulationValueCallback) {

    // Set the crossover to have the correct number of bands and the correct frequencies (TODO)
    // Use _chains rather that chains, as they may be different if chains doesn't meet DEFAULT_NUM_CHAINS
    _crossover.setNumBands(_chains.size());

    _crossover.setIsStereo(isStereo);

    // Set the processors
    for (size_t bandIndex {0}; bandIndex < _crossover.getNumBands(); bandIndex++) {
        PluginChain* newChain {_chains[bandIndex].chain.get()};
        _crossover.setPluginChain(bandIndex, newChain);
        _crossover.setIsSoloed(bandIndex, _chains[bandIndex].isSoloed);
    }
}

bool PluginSplitterMultiband::addBand() {
    bool success {false};

    if (_crossover.getNumBands() < WECore::MONSTR::Parameters::NUM_BANDS.maxValue) {
        // Create the chain first, then add the band and set the processor
        _chains.emplace_back(std::make_unique<PluginChain>(_getModulationValueCallback), false);
        _crossover.addBand();

        PluginChain* newChain {_chains[_chains.size() - 1].chain.get()};
        newChain->prepareToPlay(getSampleRate(), getBlockSize());
        _crossover.setPluginChain(_crossover.getNumBands() - 1, newChain);

        newChain->addListener(this);
        _onLatencyChange();
        success = true;
    }

     return success;
}

bool PluginSplitterMultiband::removeBand() {
    bool success {false};

    if (_crossover.getNumBands() > WECore::MONSTR::Parameters::NUM_BANDS.minValue) {
        // Remove the band first, then the chain
        _crossover.removeBand();
        _chains[_chains.size() - 1].chain->removeListener(this);
        _chains.erase(_chains.begin() + _chains.size() - 1);

        _onLatencyChange();
        success = true;
    }

     return success;
}

size_t PluginSplitterMultiband::getNumBands() {
    return _crossover.getNumBands();
}

void PluginSplitterMultiband::setCrossoverFrequency(size_t index, double val) {
    _crossover.setCrossoverFrequency(index, val);
}

double PluginSplitterMultiband::getCrossoverFrequency(size_t index) {
    return _crossover.getCrossoverFrequency(index);
}

void PluginSplitterMultiband::setChainSolo(int chainNumber, bool val) {
    // The crossover can handle soloed bands, so let it do that
    _crossover.setIsSoloed(chainNumber, val);
}

bool PluginSplitterMultiband::getChainSolo(int chainNumber) {
    return _crossover.getIsSoloed(chainNumber);
}

void PluginSplitterMultiband::prepareToPlay(double sampleRate, int samplesPerBlock) {
    _crossover.reset();
    _crossover.setSampleRate(sampleRate);
    _fftProvider.reset();
    _fftProvider.setSampleRate(sampleRate);
    PluginSplitter::prepareToPlay(sampleRate, samplesPerBlock);
}

void PluginSplitterMultiband::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) {
    _fftProvider.processBlock(buffer);
    _crossover.processBlock(buffer);
}

void PluginSplitterMultiband::_onChainRestored() {
    // Since PluginSplitter::restoreFromXml() deletes all chains at the start to make sure it has a
    // clean starting point, that can mean the first few crossover bands could still exist and
    // be pointing at chains that have been deleted. We handle this here.

    if (_chains.size() >= _crossover.getNumBands()) {
        // Need to add a new band and chain, can do this in the standard way
        addBand();
    } else {
        // We already have the bands in the crossover but the chain will have been deleted at the
        // start of PluginSplitter::restoreFromXml(), so we just need to create a new chain and
        // point the existing band to it
        _chains.emplace_back(std::make_unique<PluginChain>(_getModulationValueCallback), false);
        PluginChain* newChain {_chains[_chains.size() - 1].chain.get()};
        _crossover.setPluginChain(_chains.size() - 1, newChain);
    }

}
