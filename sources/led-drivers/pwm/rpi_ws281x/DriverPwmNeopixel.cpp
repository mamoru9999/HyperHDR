#include <led-drivers/pwm/rpi_ws281x/DriverPwmNeopixel.h>
#include <ws2811.h>

DriverPwmNeopixel::DriverPwmNeopixel(const QJsonObject& deviceConfig)
	: LedDevice(deviceConfig)
	, _ledString(nullptr)
	, _channel(0)
	, _whiteAlgorithm(RGBW::WhiteAlgorithm::HYPERSERIAL_COLD_WHITE)
	, _white_channel_limit(255)
	, _white_channel_red(255)
	, _white_channel_green(255)
	, _white_channel_blue(255)
{
}

DriverPwmNeopixel::~DriverPwmNeopixel()
{
}

LedDevice* DriverPwmNeopixel::construct(const QJsonObject& deviceConfig)
{
	return new DriverPwmNeopixel(deviceConfig);
}

bool DriverPwmNeopixel::init(const QJsonObject& deviceConfig)
{
	QString errortext;

	bool isInitOK = false;

	if (_ledString == nullptr)
		_ledString = std::unique_ptr<ws2811_t>(new ws2811_t());

	// Initialise sub-class
	if (LedDevice::init(deviceConfig))
	{
		QString whiteAlgorithm = deviceConfig["white_algorithm"].toString("hyperserial_cold_white");

		_whiteAlgorithm = RGBW::stringToWhiteAlgorithm(whiteAlgorithm);
		if (_whiteAlgorithm == RGBW::WhiteAlgorithm::INVALID)
		{
			errortext = QString("unknown white_algorithm: %1").arg(whiteAlgorithm);
			isInitOK = false;
		}
		else
		{
			_channel = deviceConfig["pwmchannel"].toInt(0);

			if (_whiteAlgorithm == RGBW::WhiteAlgorithm::HYPERSERIAL_CUSTOM)
			{
				Debug(_log, "White channel limit     : %i, red: %i, green: %i, blue: %i", _white_channel_limit, _white_channel_red, _white_channel_green, _white_channel_blue);
			}

			if (_whiteAlgorithm == RGBW::WhiteAlgorithm::HYPERSERIAL_CUSTOM ||
				_whiteAlgorithm == RGBW::WhiteAlgorithm::HYPERSERIAL_NEUTRAL_WHITE ||
				_whiteAlgorithm == RGBW::WhiteAlgorithm::HYPERSERIAL_COLD_WHITE)
			{
				RGBW::prepareRgbwCalibration(channelCorrection, _whiteAlgorithm, _white_channel_limit, _white_channel_red, _white_channel_green, _white_channel_blue);
			}

			if (_channel <0 || _channel >= RPI_PWM_CHANNELS)
			{
				errortext = QString("WS281x: invalid PWM channel. Must be greater than 0 and less than %1").arg(RPI_PWM_CHANNELS);
				isInitOK = false;
			}
			else
			{				
				memset(_ledString.get(), 0, sizeof(ws2811_t));
				_ledString->freq = deviceConfig["freq"].toInt(800000UL);
				_ledString->dmanum = deviceConfig["dma"].toInt(5);
				_ledString->channel[_channel].gpionum = deviceConfig["gpio"].toInt(18);
				_ledString->channel[_channel].count = deviceConfig["leds"].toInt(256);
				_ledString->channel[_channel].invert = deviceConfig["invert"].toInt(0);
				_ledString->channel[_channel].strip_type = (deviceConfig["rgbw"].toBool(false) ? SK6812_STRIP_GRBW : WS2811_STRIP_RGB);
				_ledString->channel[_channel].brightness = 255;

				_ledString->channel[!_channel].gpionum = 0;
				_ledString->channel[!_channel].invert = _ledString->channel[_channel].invert;
				_ledString->channel[!_channel].count = 0;
				_ledString->channel[!_channel].brightness = 0;
				_ledString->channel[!_channel].strip_type = _ledString->channel[_channel].strip_type;

				Debug(_log, "ws281x selected dma     : %d", _ledString->dmanum);
				Debug(_log, "ws281x selected channel : %d", _channel);
				Debug(_log, "ws281x total channels   : %d", RPI_PWM_CHANNELS);
				Debug(_log, "ws281x strip type       : %d", _ledString->channel[_channel].strip_type);

				if (_defaultInterval > 0)
					Warning(_log, "The refresh timer is enabled ('Refresh time' > 0) and may limit the performance of the LED driver. Ignore this error if you set it on purpose for some reason (but you almost never need it).");

				isInitOK = true;
			}
		}
	}

	if (!isInitOK)
	{
		this->setInError(errortext);
	}
	return isInitOK;
}

int DriverPwmNeopixel::open()
{
	int retval = -1;
	_isDeviceReady = false;

	if (_ledString == nullptr)
	{
		Error(_log, "Unexpected uninitialized case");
		return retval;
	}

	ws2811_return_t rc = ws2811_init(_ledString.get());
	if (rc != WS2811_SUCCESS)
	{
		QString errortext = QString("Failed to open. Error message: %1").arg(ws2811_get_return_t_str(rc));
		if (errortext.contains("mmap()", Qt::CaseInsensitive))
			errortext += ". YOUR NEED TO RUN HYPERHDR AS A ROOT USER FOR MMAP TO WORK. SUCH CONFIGURATION IS NOT OFFICIALLY SUPPORTED.";
		this->setInError(errortext);
	}
	else
	{
		_isDeviceReady = true;
		retval = 0;
	}
	return retval;
}

int DriverPwmNeopixel::close()
{
	int retval = 0;
	_isDeviceReady = false;

	// LedDevice specific closing activities
	if (isInitialised())
	{
		ws2811_fini(_ledString.get());
	}

	return retval;
}

// Send new values down the LED chain
int DriverPwmNeopixel::write(const std::vector<ColorRgb>& ledValues)
{
	int idx = 0;

	for (const ColorRgb& color : ledValues)
	{
		ColorRgbw tempRgbw;

		if (idx >= _ledString->channel[_channel].count)
		{
			break;
		}

		tempRgbw.red = color.red;
		tempRgbw.green = color.green;
		tempRgbw.blue = color.blue;
		tempRgbw.white = 0;

		if (_ledString->channel[_channel].strip_type == SK6812_STRIP_GRBW)
		{
			rgb2rgbw(color, &tempRgbw, _whiteAlgorithm, channelCorrection);
		}

		_ledString->channel[_channel].leds[idx++] =
			((uint32_t)tempRgbw.white << 24) + ((uint32_t)tempRgbw.red << 16) + ((uint32_t)tempRgbw.green << 8) + tempRgbw.blue;
	}

	while (idx < _ledString->channel[_channel].count)
	{
		_ledString->channel[_channel].leds[idx++] = 0;
	}

	return ws2811_render(_ledString.get()) ? -1 : 0;
}

bool DriverPwmNeopixel::isRegistered = hyperhdr::leds::REGISTER_LED_DEVICE("ws281x", "leds_group_1_PWM", DriverPwmNeopixel::construct);
