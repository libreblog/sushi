/**
 * @Brief General purpose multichannel audio buffer class
 * @copyright MIND Music Labs AB, Stockholm
 *
 *
 */

#ifndef SUSHI_SAMPLEBUFFER_H
#define SUSHI_SAMPLEBUFFER_H

#include <cstring>
#include <algorithm>
#include <cassert>

#include "constants.h"

namespace sushi {
//static constexpr int AUDIO_CHUNK_SIZE = 64;

template<int size>
class SampleBuffer
{
public:
    /**
     * @brief Construct a zeroed buffer with specified number of channels
     */
    SampleBuffer(int channel_count) : _channel_count(channel_count),
                                               _buffer(new float[size * channel_count])
    {
        clear();
    }

    /**
     * @brief Construct an empty buffer object with 0 channels.
     */
    SampleBuffer() noexcept : _channel_count(0),
                              _buffer(nullptr)
    {}


    /**
     * @brief Copy constructor.
     */
    SampleBuffer(const SampleBuffer &o) : _channel_count(o._channel_count),
                                          _buffer(new float[size * o._channel_count])
    {
        std::copy(o._buffer, o._buffer + (size * o._channel_count), _buffer);
    }


    /**
     * @brief Move constructor.
     */
    SampleBuffer(SampleBuffer &&o) noexcept : _channel_count(o._channel_count),
                                              _buffer(o._buffer)
    {
        o._buffer = nullptr;
    }


    /**
     * @brief Destroy the buffer.
     */
    ~SampleBuffer()
    {
        delete[] _buffer;
    }


    /**
     * @brief Assign to this buffer.
     */
    SampleBuffer &operator=(const SampleBuffer &o)
    {
        if (this != &o)  // Avoid self-assignment
        {
            if (_channel_count != o._channel_count && o._channel_count != 0)
            {
                delete[] _buffer;
                _buffer = new float[size * o._channel_count];
            }
            _channel_count = o._channel_count;
            std::copy(o._buffer, o._buffer + (size * o._channel_count), _buffer);
        }
        return *this;
    }


    /**
     * @brief Assign to this buffer using move semantics.
     */
    SampleBuffer &operator=(SampleBuffer &&o) noexcept
    {
        if (this != &o)  // Avoid self-assignment
        {
            delete[] _buffer;
            _channel_count = o._channel_count;
            _buffer = o._buffer;
            o._buffer = nullptr;
        }
        return *this;
    }

    /**
     * @brief Zero the entire buffer
     */
    void clear()
    {
        std::fill(_buffer, _buffer + (size * _channel_count), 0.0f);
    }

    /**
    * @brief Returns a writeable pointer to a specific channel in the buffer. No bounds checking.
    */
    float* channel(int channel)
    {
        return _buffer + channel * size;
    }

    /**
    * @brief Returns a read-only pointer to a specific channel in the buffer. No bounds checking.
    */
    const float* channel(int channel) const
    {
        return _buffer + channel * size;
    }

    /**
     * @brief Gets the number of channels in the buffer.
     */
    int channel_count() const
    {
        return _channel_count;
    }

    /**
     * @brief Copy interleaved audio data from interleaved_buf to this buffer.
     */
    void from_interleaved(const float* interleaved_buf)
    {
        switch (_channel_count)
        {
            case 2:  // Most common case, others are mostly included for future compatibility
            {
                float* l_in = _buffer;
                float* r_in = _buffer + size;
                for (int n = 0; n < size; ++n)
                {
                    *l_in++ = *interleaved_buf++;
                    *r_in++ = *interleaved_buf++;
                }
                break;
            }
            case 1:
            {
                std::copy(interleaved_buf, interleaved_buf + size, _buffer);
                break;
            }
            default:
            {
                for (int n = 0; n < size; ++n)
                {
                    for (int c = 0; c < _channel_count; ++c)
                    {
                        _buffer[n + c * _channel_count] = *interleaved_buf++;
                    }
                }
            }
        }
    }

    /**
     * @brief Copy buffer data in interleaved format to interleaved_buf
     */
    void to_interleaved(float* interleaved_buf)
    {
        switch (_channel_count)
        {
            case 2:  // Most common case, others are mostly included for future compatibility
            {
                float* l_out = _buffer;
                float* r_out = _buffer + size;
                for (int n = 0; n < size; ++n)
                {
                    *interleaved_buf++ = *l_out++;
                    *interleaved_buf++ = *r_out++;
                }
                break;
            }
            case 1:
            {
                std::copy(_buffer, _buffer + size, interleaved_buf);
                break;
            }
            default:
            {
                for (int n = 0; n < size; ++n)
                {
                    for (int c = 0; c < _channel_count; ++c)
                    {
                        *interleaved_buf++ = _buffer[n + c * size];
                    }
                }
            }
        }
    }

    /**
     * @brief Apply a fixed gain to the entire buffer.
     */
    void apply_gain(float gain)
    {
        for (int i = 0; i < size * _channel_count; ++i)
        {
            _buffer[i] *= gain;
        }
    }

    /**
    * @brief Apply a fixed gain to a given channel.
    */
    void apply_gain(float gain, int channel)
    {
        float* data = _buffer + size * channel;
        for (int i = 0; i < size; ++i)
        {
            data[i] *= gain;
        }
    }

    /**
     * @brief Sums the content of source into this buffer.
     *
     * source has to be either a 1 channel buffer or have the same number of channels
     * as the destination buffer.
     */
    void add(const SampleBuffer &source)
    {
        if (source.channel_count() == 1) // mono input, add to all dest channels
        {
            for (int channel = 0; channel < _channel_count; ++channel)
            {
                float* dest = _buffer + size * channel;
                for (int i = 0; i < size; ++i)
                {
                    dest[i] += source._buffer[i];
                }
            }
        } else if (source.channel_count() == _channel_count)
        {
            for (int i = 0; i < size * _channel_count; ++i)
            {
                _buffer[i] += source._buffer[i];
            }
        }
    }

    /**
     * @brief Sums one channel of source buffer into one channel of the buffer.
     */

    void add(int source_channel, int dest_channel, SampleBuffer &source)
    {
        float* source_data = source._buffer + size * source_channel;
        float* dest_data = _buffer + size * dest_channel;
        for (int i = 0; i < size; ++i)
        {
            dest_data[i] += source_data[i];
        }
    }

    /**
    * @brief Sums the content of SampleBuffer source into this buffer after applying a gain.
     *
     * source has to be either a 1 channel buffer or have the same number of channels
     * as the destination buffer.
    */
    void add_with_gain(const SampleBuffer &source, float gain)
    {
        if (source.channel_count() == 1)
        {
            for (int channel = 0; channel < _channel_count; ++channel)
            {
                float* dest = _buffer + size * channel;
                for (int i = 0; i < size; ++i)
                {
                    dest[i] += source._buffer[i] * gain;
                }
            }
        } else if (source.channel_count() == _channel_count)
        {
            for (int i = 0; i < size * _channel_count; ++i)
            {
                _buffer[i] += source._buffer[i] * gain;
            }
        }
    }

private:
    int _channel_count;
    float* _buffer;
};

} // namespace sushi


#endif //SUSHI_SAMPLEBUFFER_H