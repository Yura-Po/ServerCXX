#pragma once
#include <string>

// Sends a password-reset code to toEmail via SMTP over SSL (port 465).
// Configure credentials in smtp_sender.cpp before using.
// Returns true if the email was accepted by the SMTP server.
bool sendResetCodeEmail(const std::string& toEmail,
                        const std::string& code);
