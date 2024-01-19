#include "Pendant_WHB04B6.h"

// For Reference:
// https://github.com/LinuxCNC/linuxcnc/tree/master/src/hal/user_comps/xhc-WHB04B6
void mySend_command(const char *cmd_in)
{
    String *cmd = new String(cmd_in);
    cmd->concat("\n");
    rp2040.fifo.push_nb((uint32_t)cmd);
}

void vResetButton() {
    mySend_command("$Bye");
}

void vStopButton() {
    mySend_command("!");  
}

void vPauseRunButton() {
    mySend_command("~");  
}

void vFeedPlus() {
    mySend_command("\0x91");  
}

void vFeedMinus() {
    mySend_command("\0x92");  
}

void (*(WHB04B6BtnFx[]))() =
{
  vResetButton,     // Button RESET
  vStopButton,      // Button STOP
  vPauseRunButton,  // Button STARTPAUSE
  vFeedPlus,        // Button M1_FEEDPLUS
  vFeedMinus,       // Button M2_FEEDMINUS
  NULL,             // Button M3_SPINDLEPLUS
  NULL,             // Button M4_SPINDLEMINUS
  NULL,             // Button M5_MHOME
  NULL,             // Button M6_SAFEZ
  NULL,             // Button M7_WHOME
  NULL,             // Button M8_SPINDLEONOFF
  NULL,             // Button FN
  NULL,             // Button M9_PROBEZ
  NULL,             // Button CONTINUOUS
  NULL,             // Button STEP
  NULL              // Button M10
};

// void (*(WHB04B6BtnFx[]))() =
// {
//   vResetButton,      // Button RESET
//   vStopButton,       // Button STOP
//   vPauseRunButton,   // Button STARTPAUSE
//   NULL
// };

Pendant_WHB04B6::Pendant_WHB04B6(uint8_t dev_addr, uint8_t instance):
    USBHIDPendant(dev_addr, instance),
    axis_coordinates{0.0,0.0,0.0,0.0,0.0,0.0},
    display_report_data{ 0x06, 0xfe, 0xfd, SEED, 0x81, 0x00, 0x00, 0x00,
                          0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                          0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    axis_feed_rates{6000, 6000, 600, 6000, 6000, 6000},
    jog(0),
    selected_axis(0),
    display_axis_offset(0),
    selected_feed(0),
    mode(0x01)

{
    this->send_display_report();
    // DEMO DATA
    /*
    axis_coordinates[0]=-965.2345678;
    axis_coordinates[1]=1155.841333;
    axis_coordinates[2]=-44.8365;
    axis_coordinates[3]=34.224378;
    axis_coordinates[4]=-3335.1241333;
    axis_coordinates[5]=5564.1334465;
    */
};
void Pendant_WHB04B6::report_received(uint8_t const *report, uint16_t len)
{
    if(len != 8 || report[0] != 0x04)
        return;
    uint8_t const &random = report[1];
    uint8_t const &keycode1 = report[2];
    uint8_t const &keycode2 = report[3];
    uint8_t const &feed = report[4];
    uint8_t const &axis = report[5];
    int8_t const &jogdelta = report[6];
    uint8_t const &checksum = report[7];

    uint8_t checksum_test = (keycode1)?(random - (keycode1 ^ (~SEED & random))):(random & SEED);
    if(checksum_test != checksum)
        return;

    bool display_update_needed = (keycode1)?true:false;

    this->jog += jogdelta;

    
    if(this->selected_feed != feed)
    {
        this->selected_feed = feed;
        this->jog = 0;
    }

    if(this->selected_axis != axis)
    {
        this->selected_axis = axis;
        this->display_axis_offset = (axis>0x13)?3:0;
        if(axis>0x10 && axis<0x17)
            this->uint16_to_report_bytes(this->axis_feed_rates[axis-0x11], 16,17);
        this->jog = 0;
        display_update_needed = true;
    }

    this->process_keycodes(&keycode1, 2);

    
    if(display_update_needed)
    {
        this->send_display_report();
    }
}

void Pendant_WHB04B6::double_to_report_bytes(double val, uint8_t idx_intval_lower, uint8_t idx_intval_upper, uint8_t idx_frac_lower, uint8_t idx_frac_upper)
{
  uint16_t intval;
  uint16_t fraction;
  this->display_report_data[R_IDX(idx_frac_upper)] = 0;
  if(val<0.0)
  {
    val*=-1;
    this->display_report_data[R_IDX(idx_frac_upper)] = (1<<7);
  }
  intval = (uint16_t)val;
  fraction = (val-(float)intval)*10000;
  this->display_report_data[R_IDX(idx_intval_lower)] = intval&0xff;
  this->display_report_data[R_IDX(idx_intval_upper)] = ((intval&0xff00)>>8);
  this->display_report_data[R_IDX(idx_frac_lower)] = fraction&0xff;
  this->display_report_data[R_IDX(idx_frac_upper)] |= ((fraction&0x7f00)>>8);
}
void Pendant_WHB04B6::uint16_to_report_bytes(uint16_t val, uint8_t idx_lower, uint8_t idx_upper)
{
  this->display_report_data[R_IDX(idx_lower)] = val&0xff;
  this->display_report_data[R_IDX(idx_upper)] = ((val&0xff00)>>8);
}

void Pendant_WHB04B6::send_display_report()
{
    this->last_display_report = millis();

    // update axis coordinates in display report data
    this->double_to_report_bytes(axis_coordinates[0+this->display_axis_offset],4,5,6,7);
    this->double_to_report_bytes(axis_coordinates[1+this->display_axis_offset],8,9,10,11);
    this->double_to_report_bytes(axis_coordinates[2+this->display_axis_offset],12,13,14,15);

    // update mode indicator in display report data (mode not used yet!)
    this->display_report_data[R_IDX(3)] = (this->display_report_data[R_IDX(3)]&(~0x3)) | this->mode;

    // send display report data to device
    this->set_report(0x06, HID_REPORT_TYPE_FEATURE, &this->display_report_data, 24);
}

void Pendant_WHB04B6::loop()
{ 
  static unsigned long last_cmd_check = millis();
  unsigned long now = millis();
  if( (now-last_cmd_check) > CMD_INTERVAL)
  {
    last_cmd_check = now;
    if(this->jog != 0 && this->selected_axis>0x10 && this->selected_axis<0x17 && this->selected_feed>=0x0d && this->selected_feed<=0x10)
    {
        float multiplier = 1.0;
        uint8_t axis = this->selected_axis-0x11;
        if(this->selected_feed == 0x0d)
            multiplier = 0.001;
        else if(this->selected_feed == 0x0e)
            multiplier = 0.01;
        else if(this->selected_feed == 0x0f)
            multiplier = 0.1;
        char cmd[100];
        sprintf(cmd, WHB04B6MoveCommands[axis], this->axis_feed_rates[axis], this->jog*multiplier);
        String * cmdstr = new String(cmd);
        this->send_command(cmdstr);
    }
    this->jog = 0;
  }

  if( (now-this->last_display_report) > REPORT_INTERVAL)
  {
    this->send_display_report();
  }
}

void Pendant_WHB04B6::on_key_press(uint8_t keycode)
{
    Serial.print("Key Press: ");
    Serial.println(keycode, HEX);
    // increase or decrese feed rate
    if((keycode==KEYCODE_M1_FEEDPLUS || keycode==KEYCODE_M2_FEEDMINUS) && this->is_key_pressed(KEYCODE_FN) && this->selected_axis>0x10 && this->selected_axis<0x17)
    {
        uint8_t axis = this->selected_axis-0x11;
        if(keycode==KEYCODE_M1_FEEDPLUS)
        {
            this->axis_feed_rates[axis]+=WHB04B6FeedRateStep[axis];
            if(this->axis_feed_rates[axis]>WHB04B6FeedRateMax[axis])
                this->axis_feed_rates[axis]=WHB04B6FeedRateMax[axis];
        }
        else
        {
            if(this->axis_feed_rates[axis]>WHB04B6FeedRateStep[axis])
                this->axis_feed_rates[axis]-=WHB04B6FeedRateStep[axis];
            else
                this->axis_feed_rates[axis]=0;
        }
        // update feed rate in display report data
        this->uint16_to_report_bytes(this->axis_feed_rates[axis], 16,17);
    }
    // Send ButtonCommands
     if(keycode<=0x10)
    {
        const char* cmd;
        if (this->is_key_pressed(KEYCODE_FN))
        {
            cmd = WHB04B6ButtonCommandsFN[keycode - 1];
            if (cmd[0])
            {
                String *cmdstr = new String(cmd);
                this->send_command(cmdstr);
            }
        }
        else
            // cmd = WHB04B6ButtonCommands[keycode-1];
            // If function at index=keycode-1 is not NULL, execute function
            if ((*WHB04B6BtnFx[keycode - 1]))
                (*WHB04B6BtnFx[keycode - 1])();
            else {
                Serial.print("Not function defined for key press: ");
                Serial.println(keycode, HEX);
            }
    }
}

void Pendant_WHB04B6::on_key_release(uint8_t keycode)
{
    Serial.print("Key Release: ");
    Serial.println(keycode, HEX);
}

// void Pendant_WHB04B6::duetstatus_received(DuetStatus * duetstatus)
// {
//     for(uint8_t i=0;i<6;i++)
//         this->axis_coordinates[i] = duetstatus->axis_userPosition[i];

//     this->uint16_to_report_bytes(duetstatus->spindle_speed, 18,19);

//     this->send_display_report();
// }