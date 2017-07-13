// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.forms;

import play.data.validation.Constraints;

/**
 * This class will be used by the API validate constraints for NodeInstance data.
 */
public class NodeInstanceFormData {
  @Constraints.Required()
  public String ip;

  public int sshPort = 22;

  @Constraints.Required()
  public String sshUser;

  @Constraints.Required()
  public String region;

  @Constraints.Required()
  public String zone;

  @Constraints.Required()
  public String instanceType;

  public String nodeName;
}
