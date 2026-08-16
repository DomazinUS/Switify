/*
    Copyright 2021 XITRIX

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#include "borealis/views/cells/cell_input.hpp"
#include "borealis/views/dropdown.hpp"
#include <libretro-common/encodings/utf.h>

namespace brls
{

InputCell::InputCell()
{
    detail->setTextColor(Application::getTheme()["brls/list/listItem_value_color"]);

    BRLS_REGISTER_ENUM_XML_ATTRIBUTE(
        "type", InputCellType, this->setType,
        {
            { "text", InputCellType::TEXT },
            { "url", InputCellType::URL },
            { "password", InputCellType::PASSWORD },
        });

    this->registerClickAction([this](View* view) {
        switch (this->type) {
            case InputCellType::PASSWORD: {
                return Application::getImeManager()->openForPassword([&](std::string text) { this->setValue(text); },
                    this->title->getFullText(), this->hint, this->maxInputLength, this->value);
            }
            default: {
                return Application::getImeManager()->openForText([&](std::string text) { this->setValue(text); },
                    this->title->getFullText(), this->hint, this->maxInputLength, this->value, this->kbdDisableBitmask);
            }
        }
    });
}

void InputCell::init(std::string title, std::string value, Event<std::string>::Callback callback, std::string placeholder, std::string hint, int maxInputLength, int kbdDisableBitmask)
{
    this->hint  = hint;
    this->value = value;
    this->title->setText(title);
    this->placeholder       = placeholder;
    this->maxInputLength    = maxInputLength;
    this->kbdDisableBitmask = kbdDisableBitmask;
    this->event.subscribe(callback);
    updateUI();
}

void InputCell::setValue(std::string value)
{
    this->event.fire(value);
    this->value = value;
    updateUI();
}

void InputCell::setPlaceholder(std::string placeholder)
{
    this->placeholder = placeholder;
    updateUI();
}

void InputCell::setType(InputCellType type)
{
    this->type = type;
}

void InputCell::updateUI()
{
    Theme theme = Application::getTheme();
    if (this->value.empty())
    {
        this->detail->setText(placeholder);
        this->detail->setTextColor(theme["brls/text_disabled"]);
    }
    else if (this->type == InputCellType::PASSWORD)
    {
        std::string filled;
        size_t len = utf8len(this->value.c_str());
        for (size_t i = 0; i < len; i++) filled += "●";
        this->detail->setText(filled);
        this->detail->setTextColor(theme["brls/list/listItem_value_color"]);
    }
    else
    {
        this->detail->setText(value);
        this->detail->setTextColor(theme["brls/list/listItem_value_color"]);
    }
}

View* InputCell::create()
{
    return new InputCell();
}

InputNumericCell::InputNumericCell()
{
    detail->setTextColor(Application::getTheme()["brls/list/listItem_value_color"]);

    this->registerClickAction([this](View* view)
        {
        Application::getImeManager()->openForNumber([&](long number) {
            this->setValue(number);
        },
            this->title->getFullText(), this->hint, this->maxInputLength, std::to_string(this->value), "", "", this->kbdDisableBitmask);

        return true; });
}

void InputNumericCell::init(std::string title, long value, Event<long>::Callback callback, std::string hint, int maxInputLength, int kbdDisableBitmask)
{
    this->hint  = hint;
    this->value = value;
    this->title->setText(title);
    this->maxInputLength    = maxInputLength;
    this->kbdDisableBitmask = kbdDisableBitmask;
    this->event.subscribe(callback);
    updateUI();
}

void InputNumericCell::setValue(long value)
{
    this->event.fire(value);
    this->value = value;
    updateUI();
}

void InputNumericCell::updateUI()
{
    Theme theme = Application::getTheme();
    this->detail->setText(std::to_string(value));
    this->detail->setTextColor(theme["brls/list/listItem_value_color"]);
}

View* InputNumericCell::create()
{
    return new InputNumericCell();
}

} // namespace brls
