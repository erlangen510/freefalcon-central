// Link-only probe. Never invoke an uninitialized legacy campaign.
extern void DoCampaignLoop(int startup);
using CampaignEntry = void (*)(int);
CampaignEntry volatile campaign_entry = &DoCampaignLoop;
int main() { return campaign_entry == nullptr; }
